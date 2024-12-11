#pragma once

#if __SIZEOF_POINTER__ == 8
#  define SNMALLOC_VA_BITS_64
#else
#  define SNMALLOC_VA_BITS_32
#endif

#include <cstddef>
namespace snmalloc
{
  /**
   * Loongarch-specific architecture abstraction layer.
   */
  class AAL_LoongArch
  {
  public:
    /**
     * Bitmap of AalFeature flags
     */
    static constexpr uint64_t aal_features =
      IntegerPointers | NoCpuCycleCounters;

    static constexpr enum AalName aal_name = LoongArch;

    static constexpr size_t smallest_page_size = 0x1000;

    /**
     * On pipelined processors, notify the core that we are in a spin loop and
     * that speculative execution past this point may not be a performance gain.
     */
    static inline void pause()
    {
      __asm__ __volatile__("dbar 0" : : : "memory");
    }

    /**
     * PRELD reads a cache-line of data from memory in advance into the Cache.
     * The access address is the 12bit immediate number of the value in the
     * general register rj plus the symbol extension.
     *
     * The processor learns from the hint in the PRELD instruction what type
     * will be acquired and which level of Cache the data to be taken back fill
     * in, hint has 32 optional values (0 to 31), 0 represents load to level 1
     * Cache If the Cache attribute of the access address of the PRELD
     * instruction is not cached, then the instruction cannot generate a memory
     * access action and is treated as a NOP instruction. The PRELD instruction
     * will not trigger any exceptions related to MMU or address.
     */
    static inline void prefetch(void* ptr)
    {
      __asm__ volatile("preld 0, %0, 0" : "=r"(ptr));
    }
  };

  using AAL_Arch = AAL_LoongArch;
} // namespace snmalloc