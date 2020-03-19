#pragma once

#if defined(__aarch64__)
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
    static constexpr uint64_t aal_features =
      IntegerPointers | NoCpuCycleCounters;
    /**
     * On pipelined processors, notify the core that we are in a spin loop and
     * that speculative execution past this point may not be a performance gain.
     */
    static inline void pause()
    {
      __asm__ volatile("yield");
    }
  };

  using AAL_Arch = AAL_arm;
} // namespace snmalloc
