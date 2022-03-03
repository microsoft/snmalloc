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
     * Extra mmap flags.  Exclude mappings from core files if they are
     * pure reservations.
     */
    static int extra_mmap_flags(bool state_using)
    {
      return state_using ? 0 : MAP_PRIVATE;
    }
  };
} // namespace snmalloc
#endif
