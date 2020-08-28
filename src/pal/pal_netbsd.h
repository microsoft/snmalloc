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
     *
     * Most likely obsolete fix when the 10.x release is out.
     */
    static constexpr int default_mmap_aligned_flags =
      MAP_SHARED | MAP_ANONYMOUS;
  };
} // namespace snmalloc
#endif
