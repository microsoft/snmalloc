// SPDX-License-Identifier: MIT
//
// Heap profiler -- per-thread re-entrancy guard for the sampler slow path.
//
// Phase 2.2 of the heap-profiling milestone. Purely additive.
//
// Why: when the sampler fires a sample it walks the stack, claims a node
// from the pool, and publishes on a list. Some of those steps may transitively
// allocate (the canonical example is glibc's backtrace() which mallocs a
// thread-local buffer on first use). Re-entering the sampler from inside
// itself would either recurse infinitely or corrupt per-thread state.
//
// The guard is per-thread (TLS), POD-initialised (lives in .tbss, no
// constructor runs at first access, no __cxa_thread_atexit registration,
// no first-touch malloc). This matches the existing pattern used by
// pal_stack_walker.h's stack-bounds cache.

#pragma once

#include "../ds_core/defines.h"

#include <cstdint>

namespace snmalloc::profile
{
  /**
   * Per-thread "sampler is on the slow path" flag.
   *
   * `uint8_t` -> trivially constructible -> lives in .tbss, zero-initialised
   * by the loader / runtime; no dynamic init.
   */
  inline thread_local uint8_t profile_in_progress = 0;

  /**
   * Cheap check used by the sampler entry point to short-circuit recursive
   * entry. Returns true if the calling thread is already inside the sampler.
   */
  SNMALLOC_FAST_PATH_INLINE bool sampler_reentered() noexcept
  {
    return profile_in_progress != 0;
  }

  /**
   * RAII guard. Sets profile_in_progress on construction, clears on
   * destruction. Non-copyable / non-movable.
   *
   * Callers must check `sampler_reentered()` before constructing -- the
   * guard does not save/restore the previous value.
   */
  class ReentrancyGuard
  {
  public:
    SNMALLOC_FAST_PATH_INLINE ReentrancyGuard() noexcept
    {
      SNMALLOC_ASSERT(profile_in_progress == 0);
      profile_in_progress = 1;
    }

    SNMALLOC_FAST_PATH_INLINE ~ReentrancyGuard() noexcept
    {
      profile_in_progress = 0;
    }

    ReentrancyGuard(const ReentrancyGuard&) = delete;
    ReentrancyGuard& operator=(const ReentrancyGuard&) = delete;
    ReentrancyGuard(ReentrancyGuard&&) = delete;
    ReentrancyGuard& operator=(ReentrancyGuard&&) = delete;
  };
} // namespace snmalloc::profile
