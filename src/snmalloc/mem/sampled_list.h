#pragma once

#ifdef SNMALLOC_PROFILE

#  include <cstddef>
#  include <cstdint>
#  include <mutex>
#  include <new>

#  include "snmalloc/stl/atomic.h"

namespace snmalloc
{
  static constexpr uint32_t kMaxStackDepth = 64;

  /**
   * One live sampled allocation.
   *
   * Raw program-counter addresses are stored here; symbolication is deferred
   * to profile-dump time so there is no per-allocation symbol-lookup cost.
   */
  struct SampledAlloc
  {
    void*  ptr{nullptr};
    size_t requested_size{0};
    size_t allocated_size{0};
    size_t weight{0};
    uint32_t depth{0};
    void*  stack[kMaxStackDepth]{};

    SampledAlloc* next{nullptr};
    SampledAlloc* prev{nullptr};
  };

  /**
   * Global doubly-linked list of live sampled allocations.
   *
   * push() is lock-free (CAS on head) so it does not block the alloc path.
   * remove() and iterate() acquire a mutex; they are off the hot path.
   */
  class SampledList
  {
    stl::Atomic<SampledAlloc*> head_{nullptr};
    mutable std::mutex mutex_;

  public:
    // Called from the alloc slow path — lock-free.
    void push(SampledAlloc* node) noexcept
    {
      SampledAlloc* old = head_.load(stl::memory_order_relaxed);
      do
      {
        node->next = old;
        node->prev = nullptr;
      } while (!head_.compare_exchange_weak(
        old, node, stl::memory_order_release, stl::memory_order_relaxed));

      // Safe: node is visible to other threads via head_ before we touch
      // old->prev, and remove() holds the mutex before reading prev.
      if (old)
      {
        std::lock_guard<std::mutex> lock(mutex_);
        old->prev = node;
      }
    }

    // Called from the dealloc path — takes mutex.
    void remove(SampledAlloc* node) noexcept
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (node->prev)
        node->prev->next = node->next;
      else
        head_.store(node->next, stl::memory_order_relaxed);
      if (node->next)
        node->next->prev = node->prev;
    }

    // Snapshot iteration — takes mutex.
    template<typename Fn>
    void iterate(Fn&& fn) const
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (SampledAlloc* n = head_.load(stl::memory_order_relaxed); n;
           n = n->next)
        fn(*n);
    }
  };

  // One global list — all sampled allocations across all threads.
  inline SampledList g_sampled_list;

  /**
   * Re-entrancy guard: suppresses sampling for any allocation made
   * while we are already inside record_sample() (e.g. via backtrace()
   * calling into the allocator).
   */
  inline thread_local bool g_in_sample_recording{false};

} // namespace snmalloc

#endif // SNMALLOC_PROFILE
