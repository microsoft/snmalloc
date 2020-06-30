#pragma once

#ifdef __APPLE__
#  include "pal_bsd.h"

#  include <mach/vm_statistics.h>
#  include <utility>

namespace snmalloc
{
  /**
   * PAL implementation for Apple systems (macOS, iOS, watchOS, tvOS...).
   */
  template<int PALAnonID = PALAnonDefaultID>
  class PALApple : public PALBSD<PALApple<>>
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
     * Anonymous page tag ID.
     *
     * Darwin platform allows to gives an ID to anonymous pages via
     * the VM_MAKE_TAG's macro, from 240 up to 255 are guaranteed
     * to be free of usage, however eventually a lower could be taken
     * (e.g. LLVM sanitizers has 99) so we can monitor their states
     * via vmmap for instance. This value is provided to `mmap` as the file
     * descriptor for the mapping.
     */
    static constexpr int anonymous_memory_fd = VM_MAKE_TAG(PALAnonID);
  };
} // namespace snmalloc
#endif
