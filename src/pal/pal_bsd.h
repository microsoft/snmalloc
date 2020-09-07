#pragma once

#include "pal_posix.h"

#include <sys/mman.h>

namespace snmalloc
{
  /**
   * Generic *BSD PAL mixin.  This provides features that are common to the BSD
   * family.
   */
  template<typename OS>
  class PALBSD : public PALPOSIX<OS>
  {
  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     *
     * The generic BSD PAL does not add any features that are not supported by
     * generic POSIX systems, but explicitly declares this variable to remind
     * anyone who extends this class that they may need to modify this field.
     */
    static constexpr uint64_t pal_features = PALPOSIX<OS>::pal_features;

    /**
     * Notify platform that we will not be using these pages.
     *
     * BSD systems provide the `MADV_FREE` flag to `madvise`, which allows the
     * operating system to replace the pages with CoW copies of a zero page at
     * any point between the call and the next write to that page.
     */
    static void notify_not_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<OS::page_size>(p, size));
      madvise(p, size, MADV_FREE);
    }
  };
} // namespace snmalloc
