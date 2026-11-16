#pragma once

#include "../pal/pal_consts.h"
#include "snmalloc/stl/atomic.h"

#include <stdint.h>

#ifndef SNMALLOC_ASSIST_IDLE_MS
#  define SNMALLOC_ASSIST_IDLE_MS 1000
#endif

namespace snmalloc
{
  template<typename Config>
  inline constexpr bool uses_inactive_queue_marker =
    Config::Options.AllocIsPoolAllocated &&
    pal_supports<Time, typename Config::Pal>;

  /**
   * State and policy for deciding when to assist a disused allocator.
   */
  template<typename Config>
  class AllocPoolAssistance
  {
    /**
     * Best-effort count of inactive allocators with pending remote frees.
     * Queue-state transitions are authoritative; publication ordering may make
     * this count temporarily non-positive.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static stl::Atomic<int64_t> inactive_pending_count{0};

    /**
     * Time from which to measure the next assistance delay. Zero is an
     * ordinary timestamp, not a sentinel.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static stl::Atomic<uint64_t> idle_start_ms{0};

    /**
     * Incremented when pool acquisition claims a pending inactive allocator,
     * which restarts the assistance delay.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static stl::Atomic<uint64_t> deadline_reset_generation{0};

    /**
     * Last reset generation observed by the scheduling policy.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static stl::Atomic<uint64_t> processed_deadline_reset_generation{0};

    static bool process_deadline_reset(uint64_t sampled_time)
    {
      uint64_t processed =
        processed_deadline_reset_generation.load(stl::memory_order_acquire);
      uint64_t reset =
        deadline_reset_generation.load(stl::memory_order_acquire);
      if (processed == reset)
        return false;

      // This sample only restarts the delay; it does not reserve assistance.
      idle_start_ms.store(sampled_time, stl::memory_order_relaxed);

      // A failed CAS means another thread processed this generation. A newer
      // generation remains different and will be processed by a later sample.
      processed_deadline_reset_generation.compare_exchange_strong(
        processed, reset, stl::memory_order_release, stl::memory_order_relaxed);
      return true;
    }

  public:
    /**
     * Record an inactive allocator becoming responsible for pending remote
     * frees.
     */
    static void pending_queue_added()
    {
      inactive_pending_count.fetch_add(1, stl::memory_order_relaxed);
    }

    /**
     * Record a pending inactive allocator being claimed from the pool.
     */
    static void pending_queue_claimed()
    {
      inactive_pending_count.fetch_sub(1, stl::memory_order_relaxed);
      deadline_reset_generation.fetch_add(1, stl::memory_order_release);
    }

    /**
     * Debug-only observation of the best-effort number of inactive allocators
     * with pending remote frees.
     */
    static int64_t debug_pending_count()
    {
      return inactive_pending_count.load(stl::memory_order_relaxed);
    }

    /**
     * Return whether this sample should perform one assistance attempt.
     *
     * A successful return advances the shared pacing origin, ensuring that
     * competing threads do not assist for the same interval.
     */
    [[nodiscard]] static bool should_assist(uint64_t sampled_time)
    {
      if constexpr (uses_inactive_queue_marker<Config>)
      {
        if (process_deadline_reset(sampled_time))
          return false;

        int64_t pending =
          inactive_pending_count.load(stl::memory_order_acquire);
        if (pending <= 0)
          return false;

        // More pending queues shorten the interval between assistance attempts.
        uint64_t delay =
          uint64_t{SNMALLOC_ASSIST_IDLE_MS} / static_cast<uint64_t>(pending);
        if (delay == 0)
          delay = 1;
        uint64_t idle_start = idle_start_ms.load(stl::memory_order_relaxed);

        // Unsigned subtraction handles clock wraparound. An older sample may
        // cause an extra best-effort attempt, which is harmless.
        if ((sampled_time - idle_start) < delay)
          return false;

        // Advancing the origin reserves this interval for the winning thread.
        return idle_start_ms.compare_exchange_strong(
          idle_start,
          sampled_time,
          stl::memory_order_relaxed,
          stl::memory_order_relaxed);
      }
      else
      {
        UNUSED(sampled_time);
        return false;
      }
    }
  };
} // namespace snmalloc
