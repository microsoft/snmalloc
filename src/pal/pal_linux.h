#pragma once

#if defined(__linux__)
#  include "../ds/bits.h"
#  include "../mem/allocconfig.h"
#  include "pal_posix.h"

#  include <string.h>
#  include <sys/mman.h>

extern "C" int puts(const char* str);

namespace snmalloc
{
  class PALLinux : public PALPOSIX<PALLinux>
  {
  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     *
     * Linux does not support any features other than those in a generic POSIX
     * platform.  This field is declared explicitly to remind anyone who
     * extends this PAL that they may need to extend the set of advertised
     * features.
     */
    static constexpr uint64_t pal_features = PALPOSIX::pal_features;

    /**
     * OS specific function for zeroing memory.
     *
     * Linux implements an unusual interpretation of `MADV_DONTNEED`, which
     * immediately resets the pages to the zero state (rather than marking them
     * as sensible ones to swap out in high memory pressure).  We use this to
     * clear the underlying memory range.
     */
    template<bool page_aligned = false>
    void zero(void* p, size_t size) noexcept
    {
      // QEMU does not seem to be giving the desired behaviour for
      // MADV_DONTNEED. switch back to memset only for QEMU.
#  ifndef SNMALLOC_QEMU_WORKAROUND
      if (
        (page_aligned || is_aligned_block<OS_PAGE_SIZE>(p, size)) &&
        (size > SLAB_SIZE))
      {
        // Only use this on large allocations as memset faster, and doesn't
        // introduce IPI so faster for small allocations.
        SNMALLOC_ASSERT(is_aligned_block<OS_PAGE_SIZE>(p, size));
        madvise(p, size, MADV_DONTNEED);
      }
      else
#  endif
      {
        ::memset(p, 0, size);
      }
    }
  };
} // namespace snmalloc
#endif
