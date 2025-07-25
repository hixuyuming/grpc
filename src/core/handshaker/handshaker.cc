//
//
// Copyright 2016 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//

#include "src/core/handshaker/handshaker.h"

#include <grpc/byte_buffer.h>
#include <grpc/event_engine/event_engine.h>
#include <grpc/slice_buffer.h>
#include <grpc/support/alloc.h>
#include <grpc/support/port_platform.h>
#include <inttypes.h>

#include <string>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/event_engine_shims/endpoint.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/util/debug_location.h"
#include "src/core/util/status_helper.h"
#include "src/core/util/time.h"

using ::grpc_event_engine::experimental::EventEngine;

namespace grpc_core {

void Handshaker::InvokeOnHandshakeDone(
    HandshakerArgs* args,
    absl::AnyInvocable<void(absl::Status)> on_handshake_done,
    absl::Status status) {
  args->event_engine->Run([on_handshake_done = std::move(on_handshake_done),
                           status = std::move(status)]() mutable {
    ExecCtx exec_ctx;
    on_handshake_done(std::move(status));
    // Destroy callback while ExecCtx is still in scope.
    on_handshake_done = nullptr;
  });
}

namespace {

std::string HandshakerArgsString(HandshakerArgs* args) {
  return absl::StrFormat("{endpoint=%p, args=%s, read_buffer.Length()=%" PRIuPTR
                         ", exit_early=%d}",
                         args->endpoint.get(), args->args.ToString(),
                         args->read_buffer.Length(), args->exit_early);
}

}  // namespace

HandshakeManager::HandshakeManager()
    : RefCounted(GRPC_TRACE_FLAG_ENABLED(handshaker) ? "HandshakeManager"
                                                     : nullptr) {}

void HandshakeManager::Add(RefCountedPtr<Handshaker> handshaker) {
  MutexLock lock(&mu_);
  GRPC_TRACE_LOG(handshaker, INFO)
      << "handshake_manager " << this << ": adding handshaker "
      << std::string(handshaker->name()) << " [" << handshaker.get()
      << "] at index " << handshakers_.size();
  handshakers_.push_back(std::move(handshaker));
}

void HandshakeManager::DoHandshake(
    OrphanablePtr<grpc_endpoint> endpoint, const ChannelArgs& channel_args,
    Timestamp deadline, grpc_tcp_server_acceptor* acceptor,
    absl::AnyInvocable<void(absl::StatusOr<HandshakerArgs*>)>
        on_handshake_done) {
  // We hold a ref until after the mutex is released, because we might
  // wind up invoking on_handshake_done in another thread before we
  // return from this function, and on_handshake_done might release the
  // last ref to this object.
  auto self = Ref();
  MutexLock lock(&mu_);
  CHECK_EQ(index_, 0u);
  on_handshake_done_ = std::move(on_handshake_done);
  // Construct handshaker args.  These will be passed through all
  // handshakers and eventually be freed by the on_handshake_done callback.
  args_.endpoint = std::move(endpoint);
  args_.deadline = deadline;
  args_.args = channel_args;
  args_.event_engine = args_.args.GetObject<EventEngine>();
  args_.acceptor = acceptor;
  // Add a channelz trace that we're performing a handshake.
  // Note that we only commit this to the log if we see an error - otherwise
  // it's ephemeral and is cleaned up when refs to it are released.
  auto channelz_node = args_.args.GetObjectRef<channelz::BaseNode>();
  args_.trace_node = channelz::TraceNode(
      channelz_node.get() == nullptr
          ? channelz::ChannelTrace::Node()
          : channelz_node->NewTraceNode("Handshake connection"),
      handshaker_trace,
      [this]() { return absl::StrFormat("handshake manager %p: ", this); });
  if (acceptor != nullptr && acceptor->external_connection &&
      acceptor->pending_data != nullptr) {
    grpc_slice_buffer_swap(args_.read_buffer.c_slice_buffer(),
                           &(acceptor->pending_data->data.raw.slice_buffer));
  }
  // Start deadline timer, which owns a ref.
  const Duration time_to_deadline = deadline - Timestamp::Now();
  deadline_timer_handle_ =
      args_.event_engine->RunAfter(time_to_deadline, [self = Ref()]() mutable {
        ExecCtx exec_ctx;
        self->Shutdown(GRPC_ERROR_CREATE("Handshake timed out"));
        // HandshakeManager deletion might require an active ExecCtx.
        self.reset();
      });
  // Start first handshaker.
  CallNextHandshakerLocked(absl::OkStatus());
}

void HandshakeManager::Shutdown(absl::Status error) {
  MutexLock lock(&mu_);
  if (!is_shutdown_) {
    GRPC_CHANNELZ_LOG(args_.trace_node) << "Shutdown called: " << error;
    is_shutdown_ = true;
    // Shutdown the handshaker that's currently in progress, if any.
    if (index_ > 0) {
      GRPC_CHANNELZ_LOG(args_.trace_node)
          << "Shutting down handshaker at index " << (index_ - 1);
      handshakers_[index_ - 1]->Shutdown(std::move(error));
    }
  }
}

void HandshakeManager::CallNextHandshakerLocked(absl::Status error) {
  GRPC_TRACE_LOG(handshaker, INFO)
      << "CallNextHandshakerLocked: error=" << error
      << " shutdown=" << is_shutdown_ << " index=" << index_
      << ", args=" << HandshakerArgsString(&args_);
  CHECK(index_ <= handshakers_.size());
  // If we got an error or we've been shut down or we're exiting early or
  // we've finished the last handshaker, invoke the on_handshake_done
  // callback.
  if (!error.ok() || is_shutdown_ || args_.exit_early ||
      index_ == handshakers_.size()) {
    if (error.ok() && is_shutdown_) {
      error = GRPC_ERROR_CREATE("handshaker shutdown");
      args_.endpoint.reset();
    }
    // Since there was a handshaking error, commit this node with the reason.
    // This will make it available for inspection after the handshaker
    // completes.
    if (!error.ok()) {
      GRPC_CHANNELZ_LOG(args_.trace_node) << "Failed with error: " << error;
    }
    args_.trace_node.Commit();
    // Cancel deadline timer, since we're invoking the on_handshake_done
    // callback now.
    args_.event_engine->Cancel(deadline_timer_handle_);
    is_shutdown_ = true;
    absl::StatusOr<HandshakerArgs*> result(&args_);
    if (!error.ok()) result = std::move(error);
    args_.event_engine->Run([on_handshake_done = std::move(on_handshake_done_),
                             result = std::move(result)]() mutable {
      ExecCtx exec_ctx;
      on_handshake_done(std::move(result));
      // Destroy callback while ExecCtx is still in scope.
      on_handshake_done = nullptr;
    });
    return;
  }
  // Call the next handshaker.
  auto handshaker = handshakers_[index_];
  GRPC_CHANNELZ_LOG(args_.trace_node)
      << " calling handshaker " << handshaker->name() << " at index " << index_;
  ++index_;
  handshaker->DoHandshake(&args_, [self = Ref()](absl::Status error) mutable {
    MutexLock lock(&self->mu_);
    self->CallNextHandshakerLocked(std::move(error));
  });
}

}  // namespace grpc_core
