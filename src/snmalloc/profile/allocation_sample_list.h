// SPDX-License-Identifier: MIT
//
// Heap profiler -- streaming broadcast primitive (Phase 5.1).
//
// Distinct from `sampled_list.h` (the lock-free list of currently-live
// sampled allocations).  `AllocationSampleList` is a tiny multi-subscriber
// notification primitive: every successful `record_alloc` fan-outs an
// invocation to each registered handler.  Snapshot mode (Phase 4) keeps
// holding the SampledAlloc in `SamplerGlobals::list()` for later read; the
// streaming hook is layered on top so a process can observe every sampled
// alloc *as it happens* in addition to (or instead of) consuming snapshots
// later.
//
// Reference: tcmalloc's `MallocExtension::SetSampleHandler` -- a single
// registered C function pointer that receives each sampled alloc event in
// real time.  We support up to K=4 simultaneous subscribers (e.g. a Rust
// listener + a C++ logging shim + headroom) without dynamic allocation.
//
// Storage choice (documented per task spec):
//   We use a fixed-size std::atomic<Callback> slot array (K = 4).  This is
//   strictly simpler than an intrusive linked list (no allocation, no
//   tombstones, no ABA tagging) and matches the realistic upper bound on
//   subscribers in a heap profiler -- nobody runs four simultaneous
//   listeners in practice; we leave headroom over the tcmalloc-style "one
//   global handler".  The cost is that register() may fail with
//   `kNoFreeSlot` if all K slots are occupied; the caller surfaces that
//   to the user as the FFI's "already registered" error code.
//
// Concurrency contract:
//   - register / unregister are themselves lock-free (single CAS on a
//     slot).  They MAY race with broadcast(); broadcast tolerates a slot
//     transitioning to null mid-fan-out by checking each load.
//   - broadcast() loads each slot relaxed and invokes any non-null
//     handler.  A handler registered after broadcast has started may or
//     may not be observed -- this matches the "best-effort streaming"
//     semantics typical of sample-handlers in heap profilers.
//   - Handler invariants (REQUIRED of the caller):
//       * Must be marked `noexcept` (any exception escaping is UB).
//       * Must NOT allocate via snmalloc (would re-enter the alloc path).
//       * Must complete promptly: the handler runs on the allocating
//         thread, inline with the alloc hot path's slow arm.
//     The reentrancy ban is enforced *culturally* (header doc) rather than
//     mechanically -- but the call site in `record.h` is already inside
//     the Sampler's `ReentrancyGuard` scope, so a handler that does
//     allocate will short-circuit on its own re-entry rather than
//     infinite-loop.
//
// This file is purely additive and contains no SNMALLOC_PROFILE gating:
// it is safe to include from any TU.  The call site in record.h does the
// gating, and the FFI wiring in override/rust.cc gates with SNMALLOC_PROFILE.

#pragma once

#include "../ds_core/defines.h"
#include "sampled_alloc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace snmalloc::profile
{
  /**
   * Callback signature for streaming sample subscribers.  Invoked once per
   * sampled allocation, on the allocating thread, inside the Sampler slow
   * path's reentrancy scope.  See file-level docs for the contract.
   */
  using AllocationSampleCallback = void (*)(const SampledAlloc&) noexcept;

  /**
   * Multi-subscriber broadcast primitive for streaming-mode profiling.
   *
   * Fixed-K storage (K = kMaxSubscribers) of atomic function pointers.
   * register/unregister are single-CAS lock-free; broadcast is a tight
   * relaxed loop over the slots.
   */
  class AllocationSampleList
  {
  public:
    /// Maximum number of concurrent subscribers.  Four is comfortably
    /// above realistic usage (typically zero or one in a real heap
    /// profiler); larger values would not be useful and would add
    /// fan-out overhead to the alloc slow path.
    static constexpr size_t kMaxSubscribers = 4;

    /// Sentinel returned by register_handler / unregister_handler when
    /// the operation cannot complete.
    static constexpr int kOk = 0;
    static constexpr int kNoFreeSlot = -1;
    static constexpr int kNotRegistered = -1;

    AllocationSampleList() noexcept = default;
    AllocationSampleList(const AllocationSampleList&) = delete;
    AllocationSampleList& operator=(const AllocationSampleList&) = delete;

    /**
     * Process-wide singleton accessor.  One broadcaster per process so
     * the C FFI `sn_rust_profile_streaming_start` / `_stop` and the
     * `record_alloc` call site refer to the same registry.
     */
    static AllocationSampleList& global() noexcept
    {
      static AllocationSampleList g;
      return g;
    }

    /**
     * Register `cb` as a streaming subscriber.  Returns `kOk` on success
     * or `kNoFreeSlot` if all K slots are already in use.
     *
     * `nullptr` is rejected (would be indistinguishable from an empty
     * slot when broadcast iterates).
     */
    int register_handler(AllocationSampleCallback cb) noexcept
    {
      if (cb == nullptr)
        return kNoFreeSlot;

      for (size_t i = 0; i < kMaxSubscribers; ++i)
      {
        AllocationSampleCallback expected = nullptr;
        if (slots_[i].compare_exchange_strong(
              expected,
              cb,
              std::memory_order_acq_rel,
              std::memory_order_relaxed))
        {
          return kOk;
        }
      }
      return kNoFreeSlot;
    }

    /**
     * Remove `cb` from the subscriber set.  Returns `kOk` if a matching
     * slot was found and cleared, or `kNotRegistered` if `cb` is not
     * currently registered.
     */
    int unregister_handler(AllocationSampleCallback cb) noexcept
    {
      if (cb == nullptr)
        return kNotRegistered;

      for (size_t i = 0; i < kMaxSubscribers; ++i)
      {
        AllocationSampleCallback expected = cb;
        if (slots_[i].compare_exchange_strong(
              expected,
              nullptr,
              std::memory_order_acq_rel,
              std::memory_order_relaxed))
        {
          return kOk;
        }
      }
      return kNotRegistered;
    }

    /**
     * Fan-out a sampled-allocation event to every currently-registered
     * subscriber.  Each non-null slot is invoked exactly once in
     * (unspecified) slot order.  A null slot encountered mid-iteration
     * (because of a concurrent unregister) is simply skipped.
     *
     * The fast path -- zero subscribers -- is one relaxed load per slot.
     * On typical profile builds with no streaming consumer this is well
     * under a cache miss and falls inside the Sampler slow-path budget.
     */
    void broadcast(const SampledAlloc& sample) const noexcept
    {
      for (size_t i = 0; i < kMaxSubscribers; ++i)
      {
        AllocationSampleCallback cb = slots_[i].load(std::memory_order_acquire);
        if (cb != nullptr)
        {
          cb(sample);
        }
      }
    }

    /**
     * Test/diagnostic helper: number of currently-registered subscribers.
     * Counted with relaxed loads; intended for assertions, not for
     * branching on the hot path.
     */
    [[nodiscard]] size_t subscriber_count() const noexcept
    {
      size_t n = 0;
      for (size_t i = 0; i < kMaxSubscribers; ++i)
      {
        if (slots_[i].load(std::memory_order_relaxed) != nullptr)
          ++n;
      }
      return n;
    }

    /**
     * Test-only: clear every registered subscriber.  Not safe to call
     * concurrently with broadcast/register/unregister; intended for
     * unit-test teardown between scenarios.
     */
    void clear_all() noexcept
    {
      for (size_t i = 0; i < kMaxSubscribers; ++i)
      {
        slots_[i].store(nullptr, std::memory_order_release);
      }
    }

  private:
    alignas(kCacheLineSize)
      std::atomic<AllocationSampleCallback> slots_[kMaxSubscribers]{};
  };
} // namespace snmalloc::profile
