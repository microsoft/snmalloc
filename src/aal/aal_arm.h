#pragma once

#if defined(__arch64__)
#  define SNMALLOC_VA_BITS_64
#else
#  define SNMALLOC_VA_BITS_32
#endif

#include <iostream>
namespace snmalloc
{
  /**
   * ARM-specific architecture abstraction layer.
   */
  class AAL_arm
  {
  public:
    /**
     * Bitmap of AalFeature flags
     */
    static constexpr uint64_t aal_features = IntegerPointers;
    /**
     * On pipelined processors, notify the core that we are in a spin loop and
     * that speculative execution past this point may not be a performance gain.
     */
    static inline void pause()
    {
      __asm__ volatile("yield");
    }

    /**
     * Issue a prefetch hint at the specified address.
     */
    static inline void prefetch(void* ptr)
    {
      __builtin_prefetch(ptr, 0, 1);
    }

    /**
     * Return a cycle counter value.
     * on ARM cpu counters are accessible only in privileged mode
     */
    static inline uint64_t tick()
    {
      struct timespec n = {0, 0ul};
      clock_gettime(CLOCK_MONOTONIC, &n);

      return static_cast<uint64_t>((n.tv_sec) * (1000000000ul * n.tv_nsec));
    }
  };

  using AAL_Arch = AAL_arm;
} // namespace snmalloc
