#pragma once

#ifdef __APPLE__
#  include "pal_bsd.h"

#  include <errno.h>
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

    /**
     * Note: The root's implementation works fine on Intel
     * however mprotect/PROT_NONE fails on ARM
     * especially since the 11.2 release (seems known issue
     * spotted in various projects; might be a temporary fix).
     */
    template<bool page_aligned = false>
    static void zero(void* p, size_t size) noexcept
    {
      if constexpr (Aal::aal_name != ARM)
        PALBSD::zero(p, size);
      else
        ::bzero(p, size);
    }

    /**
     * Overriding here to mark the page as reusable
     * rolling it as much as necessary.
     * As above, the x86 h/w worked alright without this change
     * however now large allocations work better and more reliably
     * with on ARM, not to mention better RSS number accuracy
     * for tools based on task_info API.
     */
    static void notify_not_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<PALBSD::page_size>(p, size));
#  ifdef USE_POSIX_COMMIT_CHECKS
      memset(p, 0x5a, size);
#  endif
      while (madvise(p, size, MADV_FREE_REUSABLE) == -1 && errno == EAGAIN)
        ;
    }

    /**
     * same remark as above but we need to mark the page as REUSE
     * first
     */
    template<ZeroMem zero_mem>
    static void notify_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(
        is_aligned_block<PALBSD::page_size>(p, size) || (zero_mem == NoZero));
      while (madvise(p, size, MADV_FREE_REUSE) == -1 && errno == EAGAIN)
        ;

      if constexpr (zero_mem == YesZero)
        zero<true>(p, size);
    }
  };
} // namespace snmalloc
#endif
