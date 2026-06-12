// SPDX-License-Identifier: MIT
//
// Heap profiler -- record for a single sampled allocation.
//
// Phase 2.2 of the heap-profiling milestone. Purely additive: not yet wired
// into any allocator path; no SNMALLOC_PROFILE gating.
//
// See:
//   .claude/research/heap-profiling/profile-weight.md  -- weight contract
//   .claude/research/heap-profiling/synthesis.md       -- integration plan

#pragma once

#include "../ds_core/defines.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

// Stack depth captured per sample. 32 covers ~99% of stacks in C++/Rust
// release builds with inlining; see node_pool.h for the depth tradeoff.
#ifndef SNMALLOC_PROFILE_STACK_FRAMES
#  define SNMALLOC_PROFILE_STACK_FRAMES 32
#endif

namespace snmalloc::profile
{
  /// Lifecycle state of a node, stored as a single byte.
  ///   Free  -- in NodePool free-list, not on SampledList
  ///   Live  -- in NodePool acquired and published on SampledList
  ///   Freed -- removed from SampledList; awaiting return to NodePool
  enum class NodeState : uint8_t
  {
    Free = 0,
    Live = 1,
    Freed = 2,
  };

  /// Event kind tag attached to a sampled-allocation broadcast.
  ///
  /// Streaming consumers see one of:
  ///   Alloc  -- a brand-new sampled allocation (the original alloc-time
  ///             broadcast).  This is the default kind stored in the
  ///             persisted SampledList slot.
  ///   Resize -- an in-place realloc updated the size of an already-
  ///             sampled allocation.  Broadcast only; the persisted
  ///             slot's `kind` is left as `Alloc` (the sample's lifecycle
  ///             did not change -- only its size did).  The broadcast
  ///             payload carries the post-resize requested_size /
  ///             allocated_size.
  ///
  /// Out-of-place realloc (alloc + memcpy + dealloc) is NOT a Resize
  /// event: the underlying alloc-side hook already fires for the new
  /// pointer and the dealloc-side hook clears the old slot, so the
  /// streaming stream already reflects the correct lifecycle.  Resize
  /// is reserved for the in-place fast path where the existing slot is
  /// updated in place.
  enum class SampledAllocKind : uint8_t
  {
    Alloc = 0,
    Resize = 1,
  };

  static constexpr size_t MaxStackFrames = SNMALLOC_PROFILE_STACK_FRAMES;

  /// Cache-line size (matches snmalloc::CACHELINE_SIZE; duplicated here so
  /// the profile/ headers stay independent of ds_core/sizeclassconfig.h).
  static constexpr size_t kCacheLineSize = 64;

  /**
   * One sampled allocation record.
   *
   * Fields written once before publication (by the acquiring thread) and read
   * thereafter via the SampledList acquire/release link. The intrusive `next`
   * link participates in the lock-free SampledList protocol; its low bit is
   * the tombstone marker (SampledAlloc is cache-line aligned so the low bits
   * of any node pointer are free).
   *
   * Weight semantics (per profile-weight.md):
   *   `weight` is in bytes of *request* (matches tcmalloc convention).
   *   Allocated-byte view at dump time:
   *     allocated_view = weight * allocated_size / (requested_size + 1)
   *   Object-count view at dump time:
   *     count_view = weight / (requested_size + 1)
   *
   * `sample_interval_at_capture` is the sampling rate that was in force at
   * the moment this sample fired. Persisted per-node so a later rate change
   * does not retroactively misweight already-captured samples.
   */
  struct alignas(kCacheLineSize) SampledAlloc
  {
    // -- intrusive links --------------------------------------------------
    /// Tagged pointer to next node on the SampledList. Low bit = tombstone.
    /// All transitions are release on the writer and acquire on the reader.
    std::atomic<uintptr_t> next{0};

    /// NodePool free-list link. Only touched while the node is Free, under
    /// the NodePool's tagged-CAS head; no atomic needed.
    SampledAlloc* pool_next{nullptr};

    // -- payload (written once, before SampledList publication) -----------
    uintptr_t alloc_addr{0};
    size_t requested_size{0};
    size_t allocated_size{0};
    uint64_t weight{0};
    uint64_t sample_interval_at_capture{0};
    uint64_t tid{0};
    /// Monotonic acquire counter -- snapshot reader uses this to detect
    /// acquire/release races (a node freed and re-acquired between reader
    /// passes will have a different `alloc_seq`).
    uint64_t alloc_seq{0};
    /// Wall-clock nanoseconds at sample-fire, captured from
    /// `std::chrono::steady_clock` in `Sampler::record_alloc_slow`.
    /// Used by `clear_profile_slot` (the dealloc path for sampled
    /// allocations) to compute the elapsed lifetime and bump the
    /// global `LifetimeHistogram` (Phase 9.5).  Zero on nodes that
    /// were never published as part of a fired sample.
    uint64_t alloc_ts_ns{0};

    uintptr_t stack[MaxStackFrames];

    uint8_t stack_depth{0};
    /// NodeState. Atomic because the reader may consult it during a
    /// snapshot to detect a node mid-transition.
    std::atomic<uint8_t> state{static_cast<uint8_t>(NodeState::Free)};
    /// Event kind tag.  The persisted slot is always `Alloc`; a stack-
    /// local copy with `kind = Resize` is built by `record_realloc` for
    /// the streaming broadcast.  Stored as the raw uint8_t backing of
    /// `SampledAllocKind` so the struct stays POD-compatible across the
    /// FFI boundary.
    uint8_t kind{static_cast<uint8_t>(SampledAllocKind::Alloc)};
    uint8_t _pad[5]{};

    SampledAlloc() noexcept = default;
    SampledAlloc(const SampledAlloc&) = delete;
    SampledAlloc& operator=(const SampledAlloc&) = delete;

    /**
     * Clear node payload before reusing. Caller owns the node exclusively
     * (just popped off the free-list), so relaxed stores are sufficient.
     */
    SNMALLOC_FAST_PATH_INLINE void reset_for_acquire() noexcept
    {
      next.store(0, std::memory_order_relaxed);
      pool_next = nullptr;
      alloc_addr = 0;
      requested_size = 0;
      allocated_size = 0;
      weight = 0;
      sample_interval_at_capture = 0;
      tid = 0;
      alloc_seq = 0;
      alloc_ts_ns = 0;
      stack_depth = 0;
      kind = static_cast<uint8_t>(SampledAllocKind::Alloc);
      for (size_t i = 0; i < MaxStackFrames; ++i)
        stack[i] = 0;
      state.store(
        static_cast<uint8_t>(NodeState::Free), std::memory_order_relaxed);
    }
  };

  static_assert(
    alignof(SampledAlloc) >= 2,
    "SampledAlloc alignment must reserve the low bit for the tombstone tag");
} // namespace snmalloc::profile
