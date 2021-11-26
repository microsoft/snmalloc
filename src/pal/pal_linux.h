#pragma once

#if defined(__linux__)
#  include "../ds/bits.h"
#  include "pal_posix.h"

#  include <string.h>
#  include <sys/mman.h>

#  ifndef SNMALLOC_PLATFORM_HAS_GETENTROPY
#    if __has_include(<syscall.h>)
#      include <syscall.h>
#    endif
#  endif
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
    static constexpr uint64_t pal_features = PALPOSIX::pal_features | Entropy;

    static constexpr size_t page_size =
      Aal::aal_name == PowerPC ? 0x10000 : PALPOSIX::page_size;

    /**
     * Linux requires an explicit no-reserve flag in `mmap` to guarantee lazy
     * commit if /proc/sys/vm/overcommit_memory is set to `heuristic` (0).
     *
     *   https://www.kernel.org/doc/html/latest/vm/overcommit-accounting.html
     */
    static constexpr int default_mmap_flags = MAP_NORESERVE;

    /**
     * OS specific function for zeroing memory.
     *
     * Linux implements an unusual interpretation of `MADV_DONTNEED`, which
     * immediately resets the pages to the zero state (rather than marking them
     * as sensible ones to swap out in high memory pressure).  We use this to
     * clear the underlying memory range.
     */
    template<bool page_aligned = false>
    static void zero(void* p, size_t size) noexcept
    {
      // QEMU does not seem to be giving the desired behaviour for
      // MADV_DONTNEED. switch back to memset only for QEMU.
#  ifndef SNMALLOC_QEMU_WORKAROUND
      if (
        (page_aligned || is_aligned_block<page_size>(p, size)) &&
        (size > 16 * page_size))
      {
        // Only use this on large allocations as memset faster, and doesn't
        // introduce IPI so faster for small allocations.
        SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));
        madvise(p, size, MADV_DONTNEED);
      }
      else
#  endif
      {
        ::memset(p, 0, size);
      }
    }

    static void notify_not_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));

      if constexpr (PalEnforceAccess)
      {
#  if !defined(NDEBUG)
        // Fill memory so that when we switch the pages back on we don't make
        // assumptions on the content.
        memset(p, 0x5a, size);
#  endif
        madvise(p, size, MADV_FREE);
        mprotect(p, size, PROT_NONE);
      }
      else
      {
        madvise(p, size, MADV_FREE);
      }
    }

    static uint64_t get_entropy64()
    {
#  ifdef SNMALLOC_PLATFORM_HAS_GETENTROPY
      uint64_t result;
      if (getentropy(&result, sizeof(result)) != 0)
        error("Failed to get system randomness");
      return result;
#  else
      union
      {
        uint64_t result;
        char buffer[sizeof(uint64_t)];
      };
      auto current = std::begin(buffer);
      auto target = std::end(buffer);
      while (auto length = target - current)
      {
        auto ret = syscall(SYS_getrandom, current, length, 0);
        if (ret < 0)
        {
          if (errno == EINTR)
            continue;
          else
            error("Failed to get system randomness");
        }
        current += ret;
      }
      return result;
#  endif
    }
  };
} // namespace snmalloc
#endif
