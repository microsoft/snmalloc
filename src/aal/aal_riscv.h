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
      ;
      /*
       * XXX RISC V does not (yet) have a pause hint.  When it sprouts one,
       * it will be a kind of nop, so we should just add it unconditionally.
       *
       * See the "Zihintpause" proposal,
       * https://github.com/riscv/riscv-isa-manual/pull/398
       */
    }
  };

  using AAL_Arch = AAL_RISCV;
}
