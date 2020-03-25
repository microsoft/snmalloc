#pragma once

#if defined(__aarch64__)
#  define SNMALLOC_VA_BITS_64
#  ifdef _MSC_VER
#    include <arm64_neon.h>
#  endif
#else
#  define SNMALLOC_VA_BITS_32
#  ifdef _MSC_VER
#    include <arm_neon.h>
#  endif
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
    static constexpr uint64_t aal_features =
      IntegerPointers | NoCpuCycleCounters;

    /**
     * On pipelined processors, notify the core that we are in a spin loop and
     * that speculative execution past this point may not be a performance gain.
     */
    static inline void pause()
    {
#ifdef _MSC_VER
      __yield();
#else
      __asm__ volatile("yield");
#endif
    }

    static inline void prefetch(void* ptr)
    {
#ifdef _MSC_VER
      __prefetch(ptr);
#else
      __asm__ volatile("prfm pldl1keep, [%0]" : "=r"(ptr));
#endif
    }
  };

  using AAL_Arch = AAL_arm;
} // namespace snmalloc
