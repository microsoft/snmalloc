#pragma once

#if defined(__sun)
#  include "pal_posix.h"

namespace snmalloc
{
  /**
   * Platform abstraction layer for Solaris.  This provides features for this
   * system.
   */
  class PALSolaris : public PALPOSIX<PALSolaris>
  {
  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     *
     */
    static constexpr uint64_t pal_features = PALPOSIX::pal_features;

    /**
     * Solaris requires an explicit no-reserve flag in `mmap` to guarantee lazy
     * commit.
     */
    static constexpr int default_mmap_flags = MAP_NORESERVE;
  };
} // namespace snmalloc
#endif
