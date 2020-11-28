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

#include <cstddef>
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

    static constexpr enum AalName aal_name = ARM;

    static constexpr size_t smallest_page_size = 0x1000;

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
#  ifdef SNMALLOC_VA_BITS_64
      __asm__ volatile("prfm pldl1keep, [%0]" : "=r"(ptr));
#  else
      __asm__ volatile("pld\t[%0]" : "=r"(ptr));
#  endif
#endif
    }
  };

  using AAL_Arch = AAL_arm;
} // namespace snmalloc
