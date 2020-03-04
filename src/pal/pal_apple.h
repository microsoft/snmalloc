#pragma once

#ifdef __APPLE__
#  include "pal_bsd.h"

#  include <mach/vm_statistics.h>

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
     *  OS specific function for zeroing memory with the Apple application
     *  tag id.
     *
     *  See comment below.
     */
    template<bool page_aligned = false>
    void zero(void* p, size_t size)
    {
      if (page_aligned || is_aligned_block<OS_PAGE_SIZE>(p, size))
      {
        SNMALLOC_ASSERT(is_aligned_block<OS_PAGE_SIZE>(p, size));
        void* r = mmap(
          p,
          size,
          PROT_READ | PROT_WRITE,
          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
          pal_anon_id,
          0);

        if (r != MAP_FAILED)
          return;
      }

      bzero(p, size);
    }

    /**
     * Reserve memory with the Apple application tag id.
     *
     * See comment below.
     */
    template<bool committed>
    void* reserve(size_t size)
    {
      void* p = mmap(
        nullptr,
        size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        pal_anon_id,
        0);

      if (p == MAP_FAILED)
        error("Out of memory");

      return p;
    }

  private:
    /**
     * Anonymous page tag ID
     *
     * Darwin platform allows to gives an ID to anonymous pages via
     * the VM_MAKE_TAG's macro, from 240 up to 255 are guaranteed
     * to be free of usage, however eventually a lower could be taken
     * (e.g. LLVM sanitizers has 99) so we can monitor their states
     * via vmmap for instance.
     */
    static constexpr int pal_anon_id = VM_MAKE_TAG(PALAnonID);
  };
} // namespace snmalloc
#endif
