//
//
// Copyright 2015 gRPC authors.
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

#include "src/core/lib/surface/init.h"

#include <address_sorting/address_sorting.h>
#include <grpc/fork.h>
#include <grpc/grpc.h>
#include <grpc/impl/channel_arg_names.h>
#include <grpc/support/log.h>
#include <grpc/support/port_platform.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>

#include "absl/base/thread_annotations.h"
#include "absl/log/log.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "src/core/client_channel/backup_poller.h"
#include "src/core/config/core_configuration.h"
#include "src/core/credentials/transport/security_connector.h"
#include "src/core/filter/auth/auth_filters.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/event_engine/posix_engine/timer_manager.h"
#include "src/core/lib/experiments/config.h"
#include "src/core/lib/experiments/experiments.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "src/core/lib/iomgr/iomgr.h"
#include "src/core/lib/iomgr/timer_manager.h"
#include "src/core/lib/security/authorization/grpc_server_authz_filter.h"
#include "src/core/lib/surface/channel_stack_type.h"
#include "src/core/lib/surface/init_internally.h"
#include "src/core/util/fork.h"
#include "src/core/util/sync.h"
#include "src/core/util/thd.h"

// Remnants of the old plugin system
void grpc_resolver_dns_ares_init(void);
void grpc_resolver_dns_ares_shutdown(void);
void grpc_resolver_dns_ares_reset_dns_resolver(void);

extern absl::Status AresInit();
extern void AresShutdown();

#define MAX_PLUGINS 128

static gpr_once g_basic_init = GPR_ONCE_INIT;
static grpc_core::Mutex* g_init_mu;
static int g_initializations ABSL_GUARDED_BY(g_init_mu) = []() {
  grpc_core::CoreConfiguration::SetDefaultBuilder(
      grpc_core::BuildCoreConfiguration);
  return 0;
}();
static grpc_core::CondVar* g_shutting_down_cv;
static bool g_shutting_down ABSL_GUARDED_BY(g_init_mu) = false;

namespace grpc_core {
void RegisterSecurityFilters(CoreConfiguration::Builder* builder) {
  builder->channel_init()
      ->RegisterFilter<ClientAuthFilter>(GRPC_CLIENT_SUBCHANNEL)
      .IfHasChannelArg(GRPC_ARG_SECURITY_CONNECTOR);
  builder->channel_init()
      ->RegisterFilter<ClientAuthFilter>(GRPC_CLIENT_DIRECT_CHANNEL)
      .IfHasChannelArg(GRPC_ARG_SECURITY_CONNECTOR);
  builder->channel_init()
      ->RegisterFilter<ServerAuthFilter>(GRPC_SERVER_CHANNEL)
      .IfHasChannelArg(GRPC_SERVER_CREDENTIALS_ARG);
  builder->channel_init()
      ->RegisterFilter<GrpcServerAuthzFilter>(GRPC_SERVER_CHANNEL)
      .IfHasChannelArg(GRPC_ARG_AUTHORIZATION_POLICY_PROVIDER)
      .After<ServerAuthFilter>();
}
}  // namespace grpc_core

static void do_basic_init(void) {
  grpc_core::InitInternally = grpc_init;
  grpc_core::ShutdownInternally = grpc_shutdown;
  grpc_core::IsInitializedInternally = []() {
    return grpc_is_initialized() != 0;
  };
  gpr_log_verbosity_init();
  g_init_mu = new grpc_core::Mutex();
  g_shutting_down_cv = new grpc_core::CondVar();
  gpr_time_init();
  grpc_core::PrintExperimentsList();
  grpc_core::Fork::GlobalInit();
  grpc_fork_handlers_auto_register();
  grpc_tracer_init();
  grpc_client_channel_global_init_backup_polling();
}

void grpc_init(void) {
  gpr_once_init(&g_basic_init, do_basic_init);

  grpc_core::MutexLock lock(g_init_mu);
  if (++g_initializations == 1) {
    if (g_shutting_down) {
      g_shutting_down = false;
      g_shutting_down_cv->SignalAll();
    }
    grpc_iomgr_init();
    if (grpc_core::IsEventEngineDnsEnabled()) {
      address_sorting_init();
      auto status = AresInit();
      if (!status.ok()) {
        VLOG(2) << "AresInit failed: " << status.message();
      } else {
        // TODO(yijiem): remove this once we remove the iomgr dns system.
        grpc_resolver_dns_ares_reset_dns_resolver();
      }
    } else {
      grpc_resolver_dns_ares_init();
    }
    grpc_iomgr_start();
  }

  GRPC_TRACE_LOG(api, INFO) << "grpc_init(void)";
}

void grpc_shutdown_internal_locked(void)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(g_init_mu) {
  {
    grpc_core::ExecCtx exec_ctx(0);
    grpc_iomgr_shutdown_background_closure();
    grpc_timer_manager_set_threading(false);  // shutdown timer_manager thread
    if (grpc_core::IsEventEngineDnsEnabled()) {
      address_sorting_shutdown();
      AresShutdown();
    } else {
      grpc_resolver_dns_ares_shutdown();
    }
    grpc_iomgr_shutdown();
  }
  g_shutting_down = false;
  g_shutting_down_cv->SignalAll();
}

void grpc_shutdown_from_cleanup_thread(void* /*ignored*/) {
  GRPC_TRACE_LOG(api, INFO) << "grpc_shutdown_from_cleanup_thread";
  grpc_core::MutexLock lock(g_init_mu);
  // We have released lock from the shutdown thread and it is possible that
  // another grpc_init has been called, and do nothing if that is the case.
  if (--g_initializations != 0) {
    return;
  }
  grpc_shutdown_internal_locked();
  VLOG(2) << "grpc_shutdown from cleanup thread done";
}

void grpc_shutdown(void) {
  GRPC_TRACE_LOG(api, INFO) << "grpc_shutdown(void)";
  grpc_core::MutexLock lock(g_init_mu);

  if (--g_initializations == 0) {
    if (!grpc_iomgr_is_any_background_poller_thread() &&
        !grpc_event_engine::experimental::TimerManager::
            IsTimerManagerThread() &&
        grpc_core::ExecCtx::Get() == nullptr) {
      // just run clean-up when this is called on non-EventEngine thread.
      VLOG(2) << "grpc_shutdown starts clean-up now";
      g_shutting_down = true;
      grpc_shutdown_internal_locked();
      VLOG(2) << "grpc_shutdown done";
    } else {
      // spawn a detached thread to do the actual clean up in case we are
      // currently in an EventEngine thread.
      VLOG(2) << "grpc_shutdown spawns clean-up thread";
      g_initializations++;
      g_shutting_down = true;
      grpc_core::Thread cleanup_thread(
          "grpc_shutdown", grpc_shutdown_from_cleanup_thread, nullptr, nullptr,
          grpc_core::Thread::Options().set_joinable(false).set_tracked(false));
      cleanup_thread.Start();
    }
  }
}

void grpc_shutdown_blocking(void) {
  GRPC_TRACE_LOG(api, INFO) << "grpc_shutdown_blocking(void)";
  grpc_core::MutexLock lock(g_init_mu);
  if (--g_initializations == 0) {
    g_shutting_down = true;
    grpc_shutdown_internal_locked();
  }
}

int grpc_is_initialized(void) {
  int r;
  gpr_once_init(&g_basic_init, do_basic_init);
  grpc_core::MutexLock lock(g_init_mu);
  r = g_initializations > 0;
  return r;
}

void grpc_maybe_wait_for_async_shutdown(void) {
  gpr_once_init(&g_basic_init, do_basic_init);
  grpc_core::MutexLock lock(g_init_mu);
  while (g_shutting_down) {
    g_shutting_down_cv->Wait(g_init_mu);
  }
}

bool grpc_wait_for_shutdown_with_timeout(absl::Duration timeout) {
  GRPC_TRACE_LOG(api, INFO) << "grpc_wait_for_shutdown_with_timeout()";
  const auto started = absl::Now();
  const auto deadline = started + timeout;
  gpr_once_init(&g_basic_init, do_basic_init);
  grpc_core::MutexLock lock(g_init_mu);
  while (g_initializations != 0) {
    if (g_shutting_down_cv->WaitWithDeadline(g_init_mu, deadline)) {
      GRPC_TRACE_LOG(api, ERROR)
          << "grpc_wait_for_shutdown_with_timeout() timed out.";
      return false;
    }
  }
  GRPC_TRACE_LOG(api, INFO)
      << "grpc_wait_for_shutdown_with_timeout() took " << absl::Now() - started;
  return true;
}
