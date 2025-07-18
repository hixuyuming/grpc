// Copyright 2021 gRPC authors.
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

#ifndef GRPC_SRC_CORE_LIB_SLICE_SLICE_H
#define GRPC_SRC_CORE_LIB_SLICE_SLICE_H

#include <grpc/event_engine/internal/slice_cast.h>
#include <grpc/event_engine/slice.h>
#include <grpc/slice.h>
#include <grpc/support/port_platform.h>
#include <string.h>

#include <cstdint>
#include <string>
#include <utility>

#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_refcount.h"
#include "src/core/util/debug_location.h"
#include "src/core/util/string.h"

// Herein lies grpc_core::Slice and its team of thin wrappers around grpc_slice.
// They aim to keep you safe by providing strong guarantees around lifetime and
// mutability.
//
// The team:
//   Slice        - provides a wrapper around an unknown type of slice.
//                  Immutable (since we don't know who else might be referencing
//                  it), and potentially ref counted.
//   StaticSlice  - provides a wrapper around a static slice. Not refcounted,
//                  fast to copy.
//   MutableSlice - provides a guarantee of unique ownership, meaning the
//                  underlying data can be mutated safely.

// This slice implementation is an extension of the EventEngine Slice
// implementation defined in <grpc/event_engine/slice.h>. Changes to this
// implementation might warrant changes to the public EventEngine Slice
// type as well.

namespace grpc_core {

inline const grpc_slice& CSliceRef(const grpc_slice& slice,
                                   DebugLocation loc = {}) {
  if (reinterpret_cast<uintptr_t>(slice.refcount) > 1) {
    slice.refcount->Ref(loc);
  }
  return slice;
}

inline void CSliceUnref(const grpc_slice& slice, DebugLocation loc = {}) {
  if (reinterpret_cast<uintptr_t>(slice.refcount) > 1) {
    slice.refcount->Unref(loc);
  }
}

namespace slice_detail {

// Returns an empty slice.
static constexpr grpc_slice EmptySlice() { return {nullptr, {}}; }

// BaseSlice holds the grpc_slice object, but does not apply refcounting policy.
// It does export immutable access into the slice, such that this can be shared
// by all storage policies.
class BaseSlice {
 public:
  BaseSlice(const BaseSlice&) = delete;
  BaseSlice& operator=(const BaseSlice&) = delete;
  BaseSlice(BaseSlice&& other) = delete;
  BaseSlice& operator=(BaseSlice&& other) = delete;

  // Iterator access to the underlying bytes
  const uint8_t* begin() const { return GRPC_SLICE_START_PTR(c_slice()); }
  const uint8_t* end() const { return GRPC_SLICE_END_PTR(c_slice()); }
  const uint8_t* cbegin() const { return GRPC_SLICE_START_PTR(c_slice()); }
  const uint8_t* cend() const { return GRPC_SLICE_END_PTR(c_slice()); }

  // Retrieve a borrowed reference to the underlying grpc_slice.
  const grpc_slice& c_slice() const { return slice_; }

  // Retrieve the underlying grpc_slice, and replace the one in this object with
  // EmptySlice().
  grpc_slice TakeCSlice() {
    grpc_slice out = slice_;
    slice_ = EmptySlice();
    return out;
  }

  // As other things... borrowed references.
  absl::string_view as_string_view() const {
    return absl::string_view(reinterpret_cast<const char*>(data()), size());
  }

  // Array access
  uint8_t operator[](size_t i) const {
    return GRPC_SLICE_START_PTR(c_slice())[i];
  }

  // Access underlying data
  const uint8_t* data() const { return GRPC_SLICE_START_PTR(c_slice()); }

  // Size of the slice
  size_t size() const { return GRPC_SLICE_LENGTH(c_slice()); }
  size_t length() const { return size(); }
  bool empty() const { return size() == 0; }

  // For inlined slices - are these two slices equal?
  // For non-inlined slices - do these two slices refer to the same block of
  // memory?
  bool is_equivalent(const BaseSlice& other) const {
    return grpc_slice_is_equivalent(slice_, other.slice_);
  }

  uint32_t Hash() const { return grpc_slice_hash(slice_); }

 protected:
  BaseSlice() : slice_(EmptySlice()) {}
  explicit BaseSlice(const grpc_slice& slice) : slice_(slice) {}
  ~BaseSlice() = default;

  void Swap(BaseSlice* other) { std::swap(slice_, other->slice_); }
  void SetCSlice(const grpc_slice& slice) { slice_ = slice; }

  uint8_t* mutable_data() { return GRPC_SLICE_START_PTR(slice_); }

  grpc_slice* c_slice_ptr() { return &slice_; }

 private:
  grpc_slice slice_;
};

inline bool operator==(const BaseSlice& a, const BaseSlice& b) {
  return grpc_slice_eq(a.c_slice(), b.c_slice()) != 0;
}

inline bool operator!=(const BaseSlice& a, const BaseSlice& b) {
  return grpc_slice_eq(a.c_slice(), b.c_slice()) == 0;
}

inline bool operator==(const BaseSlice& a, absl::string_view b) {
  return a.as_string_view() == b;
}

inline bool operator!=(const BaseSlice& a, absl::string_view b) {
  return a.as_string_view() != b;
}

inline bool operator==(absl::string_view a, const BaseSlice& b) {
  return a == b.as_string_view();
}

inline bool operator!=(absl::string_view a, const BaseSlice& b) {
  return a != b.as_string_view();
}

inline bool operator==(const BaseSlice& a, const grpc_slice& b) {
  return grpc_slice_eq(a.c_slice(), b) != 0;
}

inline bool operator!=(const BaseSlice& a, const grpc_slice& b) {
  return grpc_slice_eq(a.c_slice(), b) == 0;
}

inline bool operator==(const grpc_slice& a, const BaseSlice& b) {
  return grpc_slice_eq(a, b.c_slice()) != 0;
}

inline bool operator!=(const grpc_slice& a, const BaseSlice& b) {
  return grpc_slice_eq(a, b.c_slice()) == 0;
}

template <typename Out>
struct CopyConstructors {
  static Out FromCopiedString(const char* s) {
    return FromCopiedBuffer(s, strlen(s));
  }
  static Out FromCopiedString(absl::string_view s) {
    return FromCopiedBuffer(s.data(), s.size());
  }
  static Out FromCopiedString(std::string s) {
    return Out(grpc_slice_from_cpp_string(std::move(s)));
  }
  static Out FromCopiedBuffer(const char* p, size_t len) {
    return Out(grpc_slice_from_copied_buffer(p, len));
  }
  static Out FromCopiedBuffer(const uint8_t* p, size_t len) {
    return Out(
        grpc_slice_from_copied_buffer(reinterpret_cast<const char*>(p), len));
  }

  template <typename Buffer>
  static Out FromCopiedBuffer(const Buffer& buffer) {
    return FromCopiedBuffer(reinterpret_cast<const char*>(buffer.data()),
                            buffer.size());
  }

  static Out FromInt64(int64_t i) {
    char buffer[GPR_LTOA_MIN_BUFSIZE];
    gpr_ltoa(i, buffer);
    return FromCopiedString(buffer);
  }
};

template <typename Out>
struct StaticConstructors {
  static Out FromStaticString(const char* s) {
    return FromStaticBuffer(s, strlen(s));
  }

  static Out FromStaticString(absl::string_view s) {
    return FromStaticBuffer(s.data(), s.size());
  }

  static Out FromStaticBuffer(const void* s, size_t len) {
    grpc_slice slice;
    slice.refcount = grpc_slice_refcount::NoopRefcount();
    slice.data.refcounted.bytes =
        const_cast<uint8_t*>(static_cast<const uint8_t*>(s));
    slice.data.refcounted.length = len;
    return Out(slice);
  }
};

}  // namespace slice_detail

class StaticSlice : public slice_detail::BaseSlice,
                    public slice_detail::StaticConstructors<StaticSlice> {
 public:
  StaticSlice() = default;
  explicit StaticSlice(const grpc_slice& slice)
      : slice_detail::BaseSlice(slice) {
    DCHECK(slice.refcount == grpc_slice_refcount::NoopRefcount());
  }

  StaticSlice(const StaticSlice& other)
      : slice_detail::BaseSlice(other.c_slice()) {}
  StaticSlice& operator=(const StaticSlice& other) {
    SetCSlice(other.c_slice());
    return *this;
  }
  StaticSlice(StaticSlice&& other) noexcept
      : slice_detail::BaseSlice(other.TakeCSlice()) {}
  StaticSlice& operator=(StaticSlice&& other) noexcept {
    Swap(&other);
    return *this;
  }
};

class GPR_MSVC_EMPTY_BASE_CLASS_WORKAROUND MutableSlice
    : public slice_detail::BaseSlice,
      public slice_detail::CopyConstructors<MutableSlice> {
 public:
  MutableSlice() = default;
  explicit MutableSlice(const grpc_slice& slice)
      : slice_detail::BaseSlice(slice) {
    DCHECK(slice.refcount == nullptr || slice.refcount->IsUnique());
  }
  ~MutableSlice() { CSliceUnref(c_slice()); }

  MutableSlice(const MutableSlice&) = delete;
  MutableSlice& operator=(const MutableSlice&) = delete;
  MutableSlice(MutableSlice&& other) noexcept
      : slice_detail::BaseSlice(other.TakeCSlice()) {}
  MutableSlice& operator=(MutableSlice&& other) noexcept {
    Swap(&other);
    return *this;
  }

  static MutableSlice CreateUninitialized(size_t length) {
    return MutableSlice(grpc_slice_malloc(length));
  }

  // Return a sub slice of this one. Leaves this slice in an indeterminate but
  // valid state.
  MutableSlice TakeSubSlice(size_t pos, size_t n) {
    return MutableSlice(grpc_slice_sub_no_ref(TakeCSlice(), pos, pos + n));
  }

  // Split this slice in two, returning the first n bytes and leaving the
  // remainder.
  MutableSlice TakeFirst(size_t n) {
    return MutableSlice(NoCheck{}, grpc_slice_split_head(c_slice_ptr(), n));
  }

  MutableSlice TakeFirstNoInline(size_t n) {
    return MutableSlice(NoCheck{},
                        grpc_slice_split_head_no_inline(c_slice_ptr(), n));
  }

  // Iterator access to the underlying bytes
  uint8_t* begin() { return mutable_data(); }
  uint8_t* end() { return mutable_data() + size(); }
  uint8_t* data() { return mutable_data(); }

  // Array access
  uint8_t& operator[](size_t i) { return mutable_data()[i]; }

  using slice_detail::BaseSlice::c_slice_ptr;

 private:
  struct NoCheck {};
  MutableSlice(NoCheck, const grpc_slice& slice)
      : slice_detail::BaseSlice(slice) {}
};

class GPR_MSVC_EMPTY_BASE_CLASS_WORKAROUND Slice
    : public slice_detail::BaseSlice,
      public slice_detail::CopyConstructors<Slice>,
      public slice_detail::StaticConstructors<Slice> {
 public:
  Slice() = default;
  ~Slice() { CSliceUnref(c_slice()); }

  explicit Slice(const grpc_slice& slice) : slice_detail::BaseSlice(slice) {}
  explicit Slice(slice_detail::BaseSlice&& other)
      : slice_detail::BaseSlice(other.TakeCSlice()) {}

  Slice(const Slice&) = delete;
  Slice& operator=(const Slice&) = delete;
  Slice(Slice&& other) noexcept : slice_detail::BaseSlice(other.TakeCSlice()) {}
  Slice& operator=(Slice&& other) noexcept {
    Swap(&other);
    return *this;
  }

  // A slice might refer to some memory that we keep a refcount to (this is
  // owned), or some memory that's inlined into the slice (also owned), or some
  // other block of memory that we know will be available for the lifetime of
  // some operation in the common case (not owned). In the *less common* case
  // that we need to keep that slice text for longer than our API's guarantee us
  // access, we need to take a copy and turn this into something that we do own.

  // TakeOwned returns an owned slice regardless of current ownership, and
  // leaves the current slice in a valid but externally unpredictable state - in
  // doing so it can avoid adding a ref to the underlying slice.
  Slice TakeOwned() {
    if (c_slice().refcount == nullptr) {
      return Slice(c_slice());
    }
    if (c_slice().refcount == grpc_slice_refcount::NoopRefcount()) {
      return Slice(grpc_slice_copy(c_slice()));
    }
    return Slice(TakeCSlice());
  }

  // As per TakeOwned, but if the slice is refcounted and there are other refs
  // then it will copy instead of ref-counting, to ensure the returned slice is
  // not shared.
  Slice TakeUniquelyOwned() {
    if (c_slice().refcount == nullptr) {
      return Slice(c_slice());
    }
    if (c_slice().refcount == grpc_slice_refcount::NoopRefcount()) {
      return Slice(grpc_slice_copy(c_slice()));
    }
    if (c_slice().refcount->IsUnique()) {
      return Slice(TakeCSlice());
    }
    return Slice(grpc_slice_copy(c_slice()));
  }

  // AsOwned returns an owned slice but does not mutate the current slice,
  // meaning that it may add a reference to the underlying slice.
  Slice AsOwned() const {
    if (c_slice().refcount == nullptr) {
      return Slice(c_slice());
    }
    if (c_slice().refcount == grpc_slice_refcount::NoopRefcount()) {
      return Slice(grpc_slice_copy(c_slice()));
    }
    return Ref();
  }

  // TakeMutable returns a MutableSlice, and leaves the current slice in an
  // indeterminate but valid state.
  // A mutable slice requires only one reference to the bytes of the slice -
  // this can be achieved either with inlined storage or with a single
  // reference.
  // If the current slice is refcounted and there are more than one references
  // to that slice, then the slice is copied in order to achieve a mutable
  // version.
  MutableSlice TakeMutable() {
    if (c_slice().refcount == nullptr) {
      return MutableSlice(c_slice());
    }
    if (c_slice().refcount != grpc_slice_refcount::NoopRefcount() &&
        c_slice().refcount->IsUnique()) {
      return MutableSlice(TakeCSlice());
    }
    return MutableSlice(grpc_slice_copy(c_slice()));
  }

  // Return a sub slice of this one. Leaves this slice in an indeterminate but
  // valid state.
  Slice TakeSubSlice(size_t pos, size_t n) {
    return Slice(grpc_slice_sub_no_ref(TakeCSlice(), pos, pos + n));
  }

  // Return a sub slice of this one. Adds a reference to the underlying slice.
  Slice RefSubSlice(size_t pos, size_t n) const {
    return Slice(grpc_slice_sub(c_slice(), pos, pos + n));
  }

  // Split this slice, returning a new slice containing (split:end] and
  // leaving this slice with [begin:split).
  Slice Split(size_t split) {
    return Slice(grpc_slice_split_tail(c_slice_ptr(), split));
  }

  Slice Ref() const { return Slice(CSliceRef(c_slice())); }

  Slice Copy() const { return Slice(grpc_slice_copy(c_slice())); }

  static Slice FromRefcountAndBytes(grpc_slice_refcount* r,
                                    const uint8_t* begin, const uint8_t* end,
                                    DebugLocation location = {}) {
    grpc_slice out;
    out.refcount = r;
    if (r != grpc_slice_refcount::NoopRefcount()) r->Ref(location);
    out.data.refcounted.bytes = const_cast<uint8_t*>(begin);
    out.data.refcounted.length = end - begin;
    return Slice(out);
  }

  static Slice FromExternalString(absl::string_view str) {
    return FromStaticString(str);
  }

  static Slice ZeroContentsWithLength(size_t length) {
    grpc_slice backing = grpc_slice_malloc(length);
    memset(GRPC_SLICE_START_PTR(backing), 0, length);
    return Slice(backing);
  }
};

}  // namespace grpc_core

namespace grpc_event_engine {
namespace experimental {
namespace internal {
template <>
struct SliceCastable<grpc_core::Slice, grpc_slice> {};
template <>
struct SliceCastable<grpc_slice, grpc_core::Slice> {};
template <>
struct SliceCastable<grpc_core::Slice, Slice> {};
template <>
struct SliceCastable<Slice, grpc_core::Slice> {};
}  // namespace internal
}  // namespace experimental
}  // namespace grpc_event_engine

#endif  // GRPC_SRC_CORE_LIB_SLICE_SLICE_H
