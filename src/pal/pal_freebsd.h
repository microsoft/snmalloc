#pragma once

#if defined(__FreeBSD__) && !defined(_KERNEL)
#  include "pal_bsd_aligned.h"

namespace snmalloc
{
  /**
   * FreeBSD-specific platform abstraction layer.
   *
   * This adds FreeBSD-specific aligned allocation to the generic BSD
   * implementation.
   */
  class PALFreeBSD : public PALBSD_Aligned<PALFreeBSD>
  {
  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     *
     * The FreeBSD PAL does not currently add any features beyond those of a
     * generic BSD with support for arbitrary alignment from `mmap`.  This
     * field is declared explicitly to remind anyone modifying this class to
     * add new features that they should add any required feature flags.
     */
    static constexpr uint64_t pal_features = PALBSD_Aligned::pal_features;
  };
} // namespace snmalloc
#endif
