// SPDX-License-Identifier: MIT
//
// Heap profiler -- pre-allocated lock-free pool of SampledAlloc nodes.
//
// Phase 2.2 of the heap-profiling milestone. Purely additive.
//
// Design:
//   - Storage is one contiguous region of Capacity SampledAlloc objects,
//     allocated via the OS directly (mmap on POSIX, VirtualAlloc on
//     Windows). We deliberately do NOT call into snmalloc's allocator
//     here -- the profile subsystem must never re-enter the host
//     allocator from inside an allocation path.
//   - Free-list is a Treiber stack with a 32-bit ABA tag in the high
//     half of a 64-bit head word and a 32-bit node index in the low half.
//   - `acquire()` returns nullptr (and bumps a drop counter) when empty;
//     the caller silently skips the sample.

#pragma once

#include "../ds_core/defines.h"
#include "sampled_alloc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

#ifndef SNMALLOC_PROFILE_POOL_CAPACITY
#  define SNMALLOC_PROFILE_POOL_CAPACITY 16384
#endif

namespace snmalloc::profile
{
  /**
   * Lock-free pool of SampledAlloc nodes with a fixed capacity.
   *
   * Thread-safe. All methods are reentry-safe: they touch only the pool's
   * own memory and call no host allocator. `init()` performs a one-shot
   * OS-level reservation on first use.
   */
  template<size_t Capacity = SNMALLOC_PROFILE_POOL_CAPACITY>
  class NodePool
  {
    static_assert(
      Capacity > 0 && Capacity < (1u << 31),
      "Capacity must fit in 31 bits (one bit reserved as null sentinel)");

  public:
    static constexpr uint32_t kNullIdx = 0xFFFFFFFFu;

    NodePool() noexcept = default;
    NodePool(const NodePool&) = delete;
    NodePool& operator=(const NodePool&) = delete;

    ~NodePool() noexcept
    {
      release_storage();
    }

    /**
     * Reserve storage and thread the free-list. Idempotent and thread-safe.
     * Safe to call from any sample-fire path.
     */
    void init() noexcept
    {
      // Cheap fast path: already initialised.
      if (SNMALLOC_LIKELY(initialized_.load(std::memory_order_acquire)))
        return;

      // Slow path: race for the right to initialise.
      bool expected = false;
      if (!initializing_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
      {
        // Lost race; spin until the winner publishes initialised_.
        while (!initialized_.load(std::memory_order_acquire))
        {
          // Tight spin: init is O(Capacity) but fast; no need for
          // anything fancier here. This is one-shot per process.
        }
        return;
      }

      const size_t bytes = Capacity * sizeof(SampledAlloc);
      void* base = os_reserve(bytes);
      if (base == nullptr)
      {
        // Stuck initialising forever is worse than visibly failing;
        // we leave initializing_ set so further callers spin (and
        // observe via drop_count when they try to acquire from the
        // never-initialised pool). The pool is unusable but the
        // process keeps going.
        initialized_.store(true, std::memory_order_release);
        return;
      }
      nodes_ = static_cast<SampledAlloc*>(base);

      // Construct each node and thread the pool_next chain.
      for (uint32_t i = 0; i < Capacity; ++i)
      {
        new (&nodes_[i]) SampledAlloc();
        nodes_[i].pool_next =
          (i + 1 == Capacity) ? nullptr : &nodes_[i + 1];
      }

      Head h{};
      h.parts.idx = 0;
      h.parts.tag = 0;
      head_.store(h.raw, std::memory_order_release);
      initialized_.store(true, std::memory_order_release);
    }

    /**
     * Pop a node off the free-list. Returns nullptr on exhaustion.
     *
     * Caller owns the returned node exclusively; it has been reset via
     * `reset_for_acquire()` and its state set to Live. The caller is
     * expected to fill payload fields and then publish it on a
     * SampledList via release-CAS.
     */
    SNMALLOC_FAST_PATH SampledAlloc* acquire() noexcept
    {
      if (SNMALLOC_UNLIKELY(!initialized_.load(std::memory_order_acquire)))
      {
        init();
        if (SNMALLOC_UNLIKELY(nodes_ == nullptr))
        {
          drops_.fetch_add(1, std::memory_order_relaxed);
          return nullptr;
        }
      }

      uint64_t cur = head_.load(std::memory_order_acquire);
      for (;;)
      {
        Head h{};
        h.raw = cur;
        if (h.parts.idx == kNullIdx)
        {
          drops_.fetch_add(1, std::memory_order_relaxed);
          return nullptr;
        }
        SampledAlloc* top = &nodes_[h.parts.idx];
        SampledAlloc* nxt = top->pool_next;
        Head nh{};
        nh.parts.idx = (nxt == nullptr)
          ? kNullIdx
          : static_cast<uint32_t>(nxt - nodes_);
        nh.parts.tag = h.parts.tag + 1;
        if (head_.compare_exchange_weak(
              cur,
              nh.raw,
              std::memory_order_acquire,
              std::memory_order_acquire))
        {
          top->reset_for_acquire();
          top->alloc_seq =
            seq_.fetch_add(1, std::memory_order_relaxed) + 1;
          top->state.store(
            static_cast<uint8_t>(NodeState::Live),
            std::memory_order_relaxed);
          return top;
        }
      }
    }

    /**
     * Push a node back on the free-list. Caller must ensure the node has
     * already been removed (tombstoned + unlinked) from any SampledList
     * before calling release().
     */
    SNMALLOC_FAST_PATH void release(SampledAlloc* n) noexcept
    {
      if (n == nullptr || nodes_ == nullptr)
        return;
      // Mark Free with release so any in-flight snapshot reader observes
      // the transition before pool_next is overwritten.
      n->state.store(
        static_cast<uint8_t>(NodeState::Free), std::memory_order_release);
      // Detach from SampledList semantics: clear the next link.
      n->next.store(0, std::memory_order_relaxed);

      const uint32_t idx = static_cast<uint32_t>(n - nodes_);
      uint64_t cur = head_.load(std::memory_order_acquire);
      for (;;)
      {
        Head h{};
        h.raw = cur;
        n->pool_next =
          (h.parts.idx == kNullIdx) ? nullptr : &nodes_[h.parts.idx];
        Head nh{};
        nh.parts.idx = idx;
        nh.parts.tag = h.parts.tag + 1;
        if (head_.compare_exchange_weak(
              cur,
              nh.raw,
              std::memory_order_release,
              std::memory_order_acquire))
          return;
      }
    }

    [[nodiscard]] uint64_t drop_count() const noexcept
    {
      return drops_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept
    {
      return Capacity;
    }

    [[nodiscard]] SampledAlloc* base() noexcept { return nodes_; }

    /**
     * Reset drops counter. Test-only helper.
     */
    void debug_reset_drops() noexcept
    {
      drops_.store(0, std::memory_order_relaxed);
    }

  private:
    /// Treiber head packed as { idx : 32, tag : 32 } in a single 64-bit word.
    union Head
    {
      struct
      {
        uint32_t idx;
        uint32_t tag;
      } parts;
      uint64_t raw;
    };
    static_assert(sizeof(Head) == 8, "Head must pack into one 64-bit word");

    static void* os_reserve(size_t bytes) noexcept
    {
#if defined(_WIN32)
      return ::VirtualAlloc(
        nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
      void* p = ::mmap(
        nullptr,
        bytes,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
      if (p == MAP_FAILED)
        return nullptr;
      return p;
#endif
    }

    static void os_release(void* base, size_t bytes) noexcept
    {
#if defined(_WIN32)
      (void)bytes;
      ::VirtualFree(base, 0, MEM_RELEASE);
#else
      ::munmap(base, bytes);
#endif
    }

    void release_storage() noexcept
    {
      if (nodes_ == nullptr)
        return;
      for (uint32_t i = 0; i < Capacity; ++i)
        nodes_[i].~SampledAlloc();
      os_release(nodes_, Capacity * sizeof(SampledAlloc));
      nodes_ = nullptr;
      initialized_.store(false, std::memory_order_release);
      initializing_.store(false, std::memory_order_release);
      Head h{};
      h.parts.idx = kNullIdx;
      h.parts.tag = 0;
      head_.store(h.raw, std::memory_order_release);
    }

    SampledAlloc* nodes_{nullptr};
    alignas(kCacheLineSize) std::atomic<uint64_t> head_{0};
    alignas(kCacheLineSize) std::atomic<uint64_t> drops_{0};
    std::atomic<uint64_t> seq_{0};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> initializing_{false};
  };
} // namespace snmalloc::profile
