#pragma once

#define SNMALLOC_VA_BITS_64

extern "C"
{
#include <machine/cheri.h>
}

namespace snmalloc
{
  /**
   * CHERI-MIPS specific architecture abstraction layer.
   */
  class AAL_CHERI_MIPS
  {
    static inline void halt_out_of_order()
    {
      static unsigned st;
      __sync_swap(&st, 0); // XXX: Overkill!
    }

    static inline uint64_t tickp()
    {
      static unsigned st;
      __sync_swap(&st, 0); // XXX: Overkill!
      return cheri_get_cyclecount();
    }

  public:
    static inline void pause()
    {
      ;
      /*
       * XXX the PAUSE instruction could be correct or not, depending on the
       * implementation details of std::atomic_flag and other callers.
       *
       * For PAUSE to be
       * correct, .test_and_set must exit with the LL flag still set and
       * .clear must store to the same word probed by test_and_set.  It
       * seems like these will be true, but they are doubtless not required
       * to be so.
       */
    }

    static inline void prefetch(void* p)
    {
      __builtin_prefetch(p, 0, 3);
    }

    static inline uint64_t tick()
    {
      return cheri_get_cyclecount();
    }

    static inline uint64_t benchmark_time_start()
    {
      halt_out_of_order();
      return AAL_Generic<AAL_CHERI_MIPS>::tick();
    }

    static inline uint64_t benchmark_time_end()
    {
      uint64_t t = AAL_Generic<AAL_CHERI_MIPS>::tickp();
      halt_out_of_order();
      return t;
    }
  };

  using AAL_Arch = AAL_CHERI_MIPS;
}
