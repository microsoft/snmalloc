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
      if (page_aligned || is_aligned_block<OS_PAGE_SIZE>(p, size))
      {
        SNMALLOC_ASSERT(is_aligned_block<OS_PAGE_SIZE>(p, size));
        madvise(p, size, MADV_DONTNEED);
      }
      else
      {
        ::memset(p, 0, size);
      }
    }
  };
} // namespace snmalloc
#endif
