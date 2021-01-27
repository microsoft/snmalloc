#pragma once

#include "pal_bsd.h"

namespace snmalloc
{
  /**
   * FreeBSD-specific platform abstraction layer.
   *
   * This adds aligned allocation using `MAP_ALIGNED` to the generic BSD
   * implementation.  This flag is supported by NetBSD and FreeBSD.
   */
  template<class OS>
  class PALBSD_Aligned : public PALBSD<OS>
  {
  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     *
     * This class adds support for aligned allocation.
     */
    static constexpr uint64_t pal_features =
      AlignedAllocation | PALBSD<OS>::pal_features;

    static constexpr size_t minimum_alloc_size = 4096;

    /**
     * Reserve memory at a specific alignment.
     */
    template<bool committed>
    static void* reserve_aligned(size_t size) noexcept
    {
      // Alignment must be a power of 2.
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(size >= minimum_alloc_size);

      size_t log2align = bits::next_pow2_bits(size);

      void* p = mmap(
        nullptr,
        size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_ALIGNED(log2align),
        -1,
        0);

      if (p == MAP_FAILED)
        PALBSD<OS>::error("Out of memory");

      return p;
    }

    /**
     * Explicitly deleted method for returning non-aligned memory.  This causes
     * incorrect use of `constexpr if` to fail on platforms with aligned
     * allocation.  Without this, this PAL and its subclasses exported both
     * allocation functions and so callers would type-check if they called
     * either in `constexpr if` branches and then fail on platforms such as
     * Linux or Windows, which expose only unaligned or aligned allocations,
     * respectively.
     */
    static std::pair<void*, size_t>
    reserve_at_least(size_t size) noexcept = delete;
  };
} // namespace snmalloc
