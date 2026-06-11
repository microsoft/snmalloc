#define SNMALLOC_NAME_MANGLE(a) sn_##a

// ---------------------------------------------------------------------------
// Profile-enabled Config wiring (Phase 4.2).
//
// When SNMALLOC_PROFILE is defined, we must replace the default
// `snmalloc::Config` (which uses NoClientMetaDataProvider) with a profile-
// enabled Config whose ClientMeta is
// `LazyArrayClientMetaDataProvider<std::atomic<SampledAlloc*>>`.  Without
// this, `config_has_profile_slot_v<Config>` is false and the alloc/dealloc
// hooks in `snmalloc/profile/record.h` compile to no-ops -- so even with
// `SNMALLOC_PROFILE=ON` no samples would ever be recorded.
//
// The pattern is the same one used by the C++ profile tests
// (e.g. src/test/func/profile_e2e/profile_e2e.cc and
// src/test/func/profile_integration/profile_integration.cc):
//
//   1. Predeclare `snmalloc::Config` as the profile-enabled type.
//   2. `#define SNMALLOC_PROVIDE_OWN_CONFIG` to suppress the default
//      typedef in `snmalloc.h`.
//   3. Pull in `snmalloc.h` (and, on the libc-API path, `malloc.cc` which
//      transitively includes `snmalloc.h` via `override.h`).
//
// When SNMALLOC_PROFILE is undefined this branch is skipped entirely and
// the shim is byte-identical to its pre-Phase-4.2 form: the default Config
// is used and the FFI hooks below collapse to the no-op stubs in the
// `#else` arm.
// ---------------------------------------------------------------------------
#ifdef SNMALLOC_PROFILE
#  include <atomic>
#  include <snmalloc/backend/globalconfig.h>
#  include <snmalloc/profile/profile.h>
#  include <snmalloc/snmalloc_core.h>

namespace snmalloc
{
  // Profile-enabled Config: stores `std::atomic<SampledAlloc*>` per
  // allocation via the lazy provider.  This flips
  // `config_has_profile_slot_v<Config>` to true, making the alloc and
  // dealloc hooks do real work and routing live samples into the
  // `SamplerGlobals::list()` consumed by the `sn_rust_profile_*` exports
  // below.
  using Config = snmalloc::StandardConfigClientMeta<
    LazyArrayClientMetaDataProvider<std::atomic<profile::SampledAlloc*>>>;
} // namespace snmalloc

#  define SNMALLOC_PROVIDE_OWN_CONFIG
#endif

// The libc API provided by malloc.cc will always be mangled per above.
#ifdef SNMALLOC_RUST_LIBC_API
#  include "malloc.cc"
#else
#  include "snmalloc/snmalloc.h"
#endif

#include "rust_profile.h"

#include <stdlib.h>
#include <string.h>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

using namespace snmalloc;

extern "C" SNMALLOC_EXPORT void*
SNMALLOC_NAME_MANGLE(rust_alloc)(size_t alignment, size_t size)
{
  return alloc(aligned_size(alignment, size));
}

extern "C" SNMALLOC_EXPORT void*
SNMALLOC_NAME_MANGLE(rust_alloc_zeroed)(size_t alignment, size_t size)
{
  return alloc<Zero>(aligned_size(alignment, size));
}

extern "C" SNMALLOC_EXPORT void
SNMALLOC_NAME_MANGLE(rust_dealloc)(void* ptr, size_t alignment, size_t size)
{
  dealloc(ptr, aligned_size(alignment, size));
}

extern "C" SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(rust_realloc)(
  void* ptr, size_t alignment, size_t old_size, size_t new_size)
{
  size_t aligned_old_size = aligned_size(alignment, old_size),
         aligned_new_size = aligned_size(alignment, new_size);
  if (
    size_to_sizeclass_full(aligned_old_size).raw() ==
    size_to_sizeclass_full(aligned_new_size).raw())
    return ptr;
  void* p = alloc(aligned_new_size);
  if (p)
  {
    memcpy(p, ptr, old_size < new_size ? old_size : new_size);
    dealloc(ptr, aligned_old_size);
  }
  return p;
}

extern "C" SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(rust_statistics)(
  size_t* current_memory_usage, size_t* peak_memory_usage)
{
  *current_memory_usage = Alloc::Config::Backend::get_current_usage();
  *peak_memory_usage = Alloc::Config::Backend::get_peak_usage();
}

extern "C" SNMALLOC_EXPORT size_t
SNMALLOC_NAME_MANGLE(rust_usable_size)(const void* ptr)
{
  return alloc_size(ptr);
}

// ---------------------------------------------------------------------------
// Heap profiling C ABI surface (Phase 4.0).
//
// These symbols are always present so the Rust FFI is linkable regardless of
// the C++ build's SNMALLOC_PROFILE setting.  When SNMALLOC_PROFILE is OFF,
// every function except `sn_rust_profile_supported` is a stub: it returns 0
// (or false / nullptr) and has no side effects.  The Rust crate may still
// expose the symbols via its own `profiling` feature gate; the two flags are
// independent so a `profiling`-enabled crate can link a non-profiling C++
// build and simply observe `supported() == false`.
//
// When SNMALLOC_PROFILE is ON, the bodies delegate to the Phase 2 / Phase 3
// machinery: snmalloc::profile::Sampler for the sampling-rate controls and
// snmalloc::profile::SamplerGlobals::list() for snapshots.  No new C++
// machinery is introduced here.
// ---------------------------------------------------------------------------

#ifdef SNMALLOC_PROFILE

namespace
{
  /**
   * Heap-allocated snapshot returned to callers as an opaque handle.
   *
   * We snapshot the SampledList into a contiguous array of plain-old-data
   * records so the caller can iterate at its leisure without holding any
   * reference into the in-process profile state.  The list itself is
   * lock-free and tolerates concurrent push/remove during the walk; we
   * copy out everything we need under the SampledList::snapshot callback.
   *
   * Backing storage uses malloc/free directly (the libc allocator that
   * snmalloc itself overrides when used as the global allocator).  This is
   * fine: snapshots are out-of-band, off the alloc hot path, and the
   * Sampler's ReentrancyGuard is not held while we are copying out.
   */
  struct RustProfileSnapshot
  {
    SnRustProfileRawSample* samples;
    size_t count;
  };
} // namespace

extern "C" SNMALLOC_EXPORT bool sn_rust_profile_supported(void)
{
  return true;
}

extern "C" SNMALLOC_EXPORT void
sn_rust_profile_set_sampling_rate(size_t bytes)
{
  snmalloc::profile::Sampler::set_sampling_rate(bytes);
}

extern "C" SNMALLOC_EXPORT size_t sn_rust_profile_get_sampling_rate(void)
{
  return snmalloc::profile::Sampler::get_sampling_rate();
}

extern "C" SNMALLOC_EXPORT void* sn_rust_profile_snapshot_begin(void)
{
  // First pass: count live samples so we know how much to allocate.
  size_t live = snmalloc::profile::SamplerGlobals::list().debug_count();

  auto* snap = static_cast<RustProfileSnapshot*>(
    ::malloc(sizeof(RustProfileSnapshot)));
  if (snap == nullptr)
    return nullptr;

  snap->samples = nullptr;
  snap->count = 0;

  if (live == 0)
    return snap;

  // We may race against concurrent pushes that grow the list between
  // the count above and the copy below.  Allocate a slight overshoot to
  // absorb a small burst, then bound the actual copy by both the buffer
  // capacity and the SampledList's live count at copy time.  Anything
  // that arrives after the snapshot starts is simply not observed --
  // that is the standard semantics for a heap-profiler snapshot.
  const size_t cap = live + 16;
  snap->samples = static_cast<SnRustProfileRawSample*>(
    ::malloc(cap * sizeof(SnRustProfileRawSample)));
  if (snap->samples == nullptr)
  {
    ::free(snap);
    return nullptr;
  }

  size_t idx = 0;
  snmalloc::profile::SamplerGlobals::list().snapshot(
    [&](snmalloc::profile::SampledAlloc* node) noexcept {
      if (idx >= cap)
        return;
      SnRustProfileRawSample& out = snap->samples[idx];
      out.alloc_ptr = reinterpret_cast<void*>(node->alloc_addr);
      out.requested_size = node->requested_size;
      out.allocated_size = node->allocated_size;
      out.weight = static_cast<size_t>(node->weight);
      const size_t depth =
        node->stack_depth <= SNMALLOC_PROFILE_STACK_FRAMES
        ? node->stack_depth
        : SNMALLOC_PROFILE_STACK_FRAMES;
      out.stack_depth = static_cast<uint32_t>(depth);
      for (size_t i = 0; i < depth; ++i)
        out.stack[i] = reinterpret_cast<void*>(node->stack[i]);
      for (size_t i = depth; i < SNMALLOC_PROFILE_STACK_FRAMES; ++i)
        out.stack[i] = nullptr;
      ++idx;
    });

  snap->count = idx;
  return snap;
}

extern "C" SNMALLOC_EXPORT size_t sn_rust_profile_snapshot_count(void* handle)
{
  if (handle == nullptr)
    return 0;
  return static_cast<RustProfileSnapshot*>(handle)->count;
}

extern "C" SNMALLOC_EXPORT bool sn_rust_profile_snapshot_get(
  void* handle, size_t idx, SnRustProfileRawSample* out)
{
  if (handle == nullptr || out == nullptr)
    return false;
  auto* snap = static_cast<RustProfileSnapshot*>(handle);
  if (idx >= snap->count)
    return false;
  *out = snap->samples[idx];
  return true;
}

extern "C" SNMALLOC_EXPORT void sn_rust_profile_snapshot_end(void* handle)
{
  if (handle == nullptr)
    return;
  auto* snap = static_cast<RustProfileSnapshot*>(handle);
  ::free(snap->samples);
  ::free(snap);
}

// ---------------------------------------------------------------------------
// Streaming-mode FFI (Phase 5.1).
//
// We expose a single registered C callback that receives one event per
// sampled allocation, mirroring tcmalloc's MallocExtension::SetSampleHandler.
// Internally the broadcast primitive
// (snmalloc::profile::AllocationSampleList) supports up to K=4 concurrent
// subscribers, but the FFI surface is intentionally restricted to a single
// process-wide handler: returning -1 on "already registered" keeps the
// Rust-facing contract drama-free (no slot index to track) and matches the
// tcmalloc precedent.  A user that needs multiple subscribers can register
// at the C++ level directly.
//
// The shim converts each in-flight `SampledAlloc` to the FFI-stable
// `SnRustProfileRawSample` POD before invoking the user callback -- the
// user never observes the C++ type.  The shim itself is `noexcept` and
// performs no allocation, satisfying the AllocationSampleList handler
// contract.
// ---------------------------------------------------------------------------

namespace
{
  /// Single registered user callback for streaming mode.  Stored as an
  /// atomic so the broadcast thread always observes a coherent value.
  /// Distinct from the AllocationSampleList slots: the FFI shim
  /// `streaming_broadcast_shim` lives in one slot of the broadcast list,
  /// and that shim in turn dispatches through this pointer.
  std::atomic<void (*)(const SnRustProfileRawSample*)> g_streaming_user_cb{
    nullptr};

  /**
   * Bridge function registered with AllocationSampleList::global(); copies
   * the live SampledAlloc into the FFI-stable POD and invokes the user
   * callback.  Marked `noexcept` per the AllocationSampleCallback contract.
   */
  void streaming_broadcast_shim(
    const snmalloc::profile::SampledAlloc& node) noexcept
  {
    auto user_cb = g_streaming_user_cb.load(std::memory_order_acquire);
    if (user_cb == nullptr)
      return;

    // Stack-local sample -- no allocation on the hot path, matching the
    // "no allocator re-entry" contract documented on
    // AllocationSampleCallback.
    SnRustProfileRawSample out{};
    out.alloc_ptr = reinterpret_cast<void*>(node.alloc_addr);
    out.requested_size = node.requested_size;
    out.allocated_size = node.allocated_size;
    out.weight = static_cast<size_t>(node.weight);
    const size_t depth = node.stack_depth <= SNMALLOC_PROFILE_STACK_FRAMES
      ? node.stack_depth
      : SNMALLOC_PROFILE_STACK_FRAMES;
    out.stack_depth = static_cast<uint32_t>(depth);
    for (size_t i = 0; i < depth; ++i)
      out.stack[i] = reinterpret_cast<void*>(node.stack[i]);
    for (size_t i = depth; i < SNMALLOC_PROFILE_STACK_FRAMES; ++i)
      out.stack[i] = nullptr;

    user_cb(&out);
  }
} // namespace

extern "C" SNMALLOC_EXPORT int sn_rust_profile_streaming_start(
  void (*cb)(const SnRustProfileRawSample*))
{
  if (cb == nullptr)
    return -1;

  // Reject re-registration: a single user callback is allowed at a time
  // through the FFI.  CAS from null -> cb; failure means a previous
  // start() is still active.
  void (*expected)(const SnRustProfileRawSample*) = nullptr;
  if (!g_streaming_user_cb.compare_exchange_strong(
        expected, cb, std::memory_order_acq_rel, std::memory_order_relaxed))
  {
    return -1;
  }

  const int rc = snmalloc::profile::AllocationSampleList::global()
                   .register_handler(streaming_broadcast_shim);
  if (rc != snmalloc::profile::AllocationSampleList::kOk)
  {
    // Couldn't register the shim (all slots full from C++-side
    // subscribers).  Roll back the user-callback store so a subsequent
    // start() can try again, then fail.
    g_streaming_user_cb.store(nullptr, std::memory_order_release);
    return -1;
  }
  return 0;
}

extern "C" SNMALLOC_EXPORT int sn_rust_profile_streaming_stop(void)
{
  // Unregister the shim first; from this point no further broadcasts
  // will dispatch to the user callback.  Order matters here because
  // record_alloc holds no mutex around the broadcast call -- an
  // in-flight broadcast loaded the shim before we unregistered will
  // still observe a non-null user_cb until we clear that next.
  const int rc = snmalloc::profile::AllocationSampleList::global()
                   .unregister_handler(streaming_broadcast_shim);

  auto prev = g_streaming_user_cb.exchange(nullptr, std::memory_order_acq_rel);

  if (rc != snmalloc::profile::AllocationSampleList::kOk || prev == nullptr)
    return -1;
  return 0;
}

#else // !SNMALLOC_PROFILE

// Stubs: keep the FFI surface linkable when profiling is compiled out.

extern "C" SNMALLOC_EXPORT bool sn_rust_profile_supported(void)
{
  return false;
}

extern "C" SNMALLOC_EXPORT void
sn_rust_profile_set_sampling_rate(size_t /*bytes*/)
{
}

extern "C" SNMALLOC_EXPORT size_t sn_rust_profile_get_sampling_rate(void)
{
  return 0;
}

extern "C" SNMALLOC_EXPORT void* sn_rust_profile_snapshot_begin(void)
{
  return nullptr;
}

extern "C" SNMALLOC_EXPORT size_t sn_rust_profile_snapshot_count(void* /*h*/)
{
  return 0;
}

extern "C" SNMALLOC_EXPORT bool sn_rust_profile_snapshot_get(
  void* /*handle*/, size_t /*idx*/, SnRustProfileRawSample* /*out*/)
{
  return false;
}

extern "C" SNMALLOC_EXPORT void sn_rust_profile_snapshot_end(void* /*h*/)
{
}

extern "C" SNMALLOC_EXPORT int sn_rust_profile_streaming_start(
  void (*)(const SnRustProfileRawSample*))
{
  return -1;
}

extern "C" SNMALLOC_EXPORT int sn_rust_profile_streaming_stop(void)
{
  return -1;
}

#endif // SNMALLOC_PROFILE
