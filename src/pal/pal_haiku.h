#pragma once

#if defined(__HAIKU__)
#  include "pal_posix.h"

#  include <sys/mman.h>

namespace snmalloc
{
  /**
   * Platform abstraction layer for Haiku.  This provides features for this
   * system.
   */
  class PALHaiku : public PALPOSIX<PALHaiku>
  {
  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     *
     */
    static constexpr uint64_t pal_features = PALPOSIX::pal_features;

    /**
     * Notify platform that we will not be needing these pages.
     * Haiku does not provide madvise call per say only the posix equivalent.
     */
    void notify_not_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));
      posix_madvise(p, size, POSIX_MADV_DONTNEED);
    }

    /**
     *  OS specific function for zeroing memory
     *  using MAP_NORESERVE for explicit over commit appliance.
     */
    template<bool page_aligned = false>
    void zero(void* p, size_t size)
    {
      if (page_aligned || is_aligned_block<page_size>(p, size))
      {
        SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));
        void* r = mmap(
          p,
          size,
          PROT_READ | PROT_WRITE,
          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
          -1,
          0);

        if (r != MAP_FAILED)
          return;
      }

      bzero(p, size);
    }

    /**
     * Reserve memory using MAP_NORESERVE for explicit
     * over commit applicance.
     */
    std::pair<void*, size_t> reserve_at_least(size_t size)
    {
      constexpr size_t min_size =
        bits::is64() ? bits::one_at_bit(32) : bits::one_at_bit(28);
      auto size_request = bits::max(size, min_size);

      void* p = mmap(
        nullptr,
        size_request,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
        -1,
        0);

      if (p == MAP_FAILED)
        error("Out of memory");

      return {p, size_request};
    }
  };
} // namespace snmalloc
#endif
