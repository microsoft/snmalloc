#pragma once

#if defined(__OpenBSD__) && !defined(_KERNEL)
#  include "pal_bsd.h"

namespace snmalloc
{
  /**
   * OpenBSD platform abstraction layer.
   *
   * OpenBSD behaves exactly like a generic BSD platform but this class exists
   * as a place to add OpenBSD-specific behaviour later, if required.
   */
  class PALOpenBSD : public PALBSD<PALOpenBSD>
  {
  public:
    /**
     * The features exported by this PAL.
     *
     * Currently, these are identical to the generic BSD PAL.  This field is
     * declared explicitly to remind anyone who modifies this class that they
     * should add any required features.
     */
    static constexpr uint64_t pal_features = PALBSD::pal_features;

    /**
     *
     * On OpenBSD, pages seem not discarded in due time
     * despite normally MADV_FREE is normally fast enough
     * in other platforms, thus some unit tests are failing.
     *
     * Notifying by discarding access instead.
     */

    static void notify_not_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<PALOpenBSD::page_size>(p, size));
      mprotect(p, size, PROT_NONE);
    }

    template<ZeroMem zero_mem>
    static void notify_using(void* p, size_t size)
    {
      SNMALLOC_ASSERT(is_aligned_block<PALOpenBSD::page_size>(p, size));
      mprotect(p, size, PROT_READ | PROT_WRITE);
    }
  };
} // namespace snmalloc
#endif
