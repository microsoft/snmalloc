#pragma once

#if __riscv_xlen == 64
#  define SNMALLOC_VA_BITS_64
#elif __riscv_xlen == 32
#  define SNMALLOC_VA_BITS_32
#endif

namespace snmalloc
{
  /**
   * RISC-V architecture layer, phrased as generically as possible.  Specific
   * implementations may need to adjust some of these.
   */
  class AAL_RISCV
  {
  public:
    static constexpr uint64_t aal_features = IntegerPointers;

    static constexpr size_t smallest_page_size = 0x1000;

    static constexpr AalName aal_name = RISCV;

    static void inline pause()
    {
      // The "Zihintpause" extension steals some dead space of the "fence"
      // instruction and so should be available everywhere even if it doesn't do
      // anything on a particular microarchitecture.  Our assemblers don't
      // understand it, yet, tho', so thus this hilarity.
      __asm__ volatile(".byte 0xF; .byte 0x0; .byte 0x0; .byte 0x1");
    }
  };

  using AAL_Arch = AAL_RISCV;
}
