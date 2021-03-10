#pragma once

#if _MIPS_SIM == _ABI64
#  define SNMALLOC_VA_BITS_64
#else
#  define SNMALLOC_VA_BITS_32
#endif

namespace snmalloc
{
  /**
   * MIPS architecture layer, phrased as generically as possible.  Specific
   * MIPS implementations may need to adjust some of these.
   */
  class AAL_MIPS
  {
  public:
    static constexpr uint64_t aal_features = IntegerPointers;

    static constexpr size_t smallest_page_size = 0x1000;

    static constexpr int aal_name = MIPS;

    static void inline pause()
    {
#if _MIPS_SIM != _ABI64
      /*
       * The PAUSE instruction (MIPS64 II-A v6.05, page 374) could be exactly
       * what we want, or not, depending on the implementation details of
       * std::atomic_flag and other callers.
       *
       * For PAUSE to actually pause, .test_and_set must exit with the LL flag
       * still set and .clear must store to the same word probed by
       * test_and_set.  It seems like these will be true, but they are
       * doubtless not required to be so, in which case, pause will just be a
       * NOP.
       *
       * Oddly, PAUSE seems only available on MIPS32. :(
       */
      __asm__ volatile("pause");
#endif
    }
  };

  using AAL_Arch = AAL_MIPS;
}
