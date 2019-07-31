#pragma once

#if defined(__FreeBSD__) && !defined(_KERNEL)
#  include "../ds/bits.h"
#  include "../mem/allocconfig.h"
#  include "pal_bsd.h"

#  include <stdio.h>
#  include <strings.h>
#  include <sys/mman.h>

namespace snmalloc
{
  class PALFBSD : public PALBSD
  {
  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     */
    static constexpr uint64_t pal_features = AlignedAllocation | LazyCommit;

    template<bool committed>
    void* reserve(const size_t* size, size_t align) noexcept
    {
      size_t request = *size;
      // Alignment must be a power of 2.
      assert(align == bits::next_pow2(align));

      align = bits::max<size_t>(4096, align);

      size_t log2align = bits::next_pow2_bits(align);

      void* p = mmap(
        nullptr,
        request,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_ALIGNED(log2align),
        -1,
        0);

      if (p == MAP_FAILED)
        error("Out of memory");

      return p;
    }
  };
} // namespace snmalloc
#endif
