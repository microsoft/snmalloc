#pragma once

#ifdef __NetBSD__
#  include "pal_bsd_aligned.h"

namespace snmalloc
{
  /**
   * NetBSD-specific platform abstraction layer.
   *
   * This adds NetBSD-specific aligned allocation to the generic BSD
   * implementation.
   */
  class PALNetBSD : public PALBSD_Aligned<PALNetBSD>
  {
  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     *
     * The NetBSD PAL does not currently add any features beyond those of a
     * generic BSD with support for arbitrary alignment from `mmap`.  This
     * field is declared explicitly to remind anyone modifying this class to
     * add new features that they should add any required feature flags.
     */
    static constexpr uint64_t pal_features = PALBSD_Aligned::pal_features;

    /**
     * Reserve memory at a specific alignment.
     * Oddily, it appears it needs to be shared segment
     * as it should not need to but func-memory-16 crashes otherwise.
     */
    template<bool committed>
    void* reserve_aligned(size_t size) noexcept
    {
      // Alignment must be a power of 2.
      SNMALLOC_ASSERT(size == bits::next_pow2(size));
      SNMALLOC_ASSERT(size >= minimum_alloc_size);

      size_t log2align = bits::next_pow2_bits(size);

      void* p = mmap(
        nullptr,
        size,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS | MAP_ALIGNED(log2align),
        -1,
        0);

      if (p == MAP_FAILED)
        PALBSD_Aligned<PALNetBSD>::error("Out of memory");

      return p;
    }
  };
} // namespace snmalloc
#endif
