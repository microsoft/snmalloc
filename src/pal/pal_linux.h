#pragma once

#if defined(__linux__)
#  include "../ds/bits.h"
#  include "../mem/allocconfig.h"

#  include <string.h>
#  include <sys/mman.h>

extern "C" int puts(const char* str);

namespace snmalloc
{
  class PALLinux
  {
  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     */
    static constexpr uint64_t pal_features = 0;
    static void error(const char* const str)
    {
      puts(str);
      abort();
    }

    /// Notify platform that we will not be using these pages
    void notify_not_using(void* p, size_t size) noexcept
    {
      assert(bits::is_aligned_block<OS_PAGE_SIZE>(p, size));
      // Do nothing. Don't call madvise here, as the system call slows the
      // allocator down too much.
      UNUSED(p);
      UNUSED(size);
    }

    /// Notify platform that we will be using these pages
    template<ZeroMem zero_mem>
    void notify_using(void* p, size_t size) noexcept
    {
      assert(
        bits::is_aligned_block<OS_PAGE_SIZE>(p, size) || (zero_mem == NoZero));

      if constexpr (zero_mem == YesZero)
        zero<true>(p, size);
      else
      {
        UNUSED(p);
        UNUSED(size);
      }
    }

    /// OS specific function for zeroing memory
    template<bool page_aligned = false>
    void zero(void* p, size_t size) noexcept
    {
      if (page_aligned || bits::is_aligned_block<OS_PAGE_SIZE>(p, size))
      {
        assert(bits::is_aligned_block<OS_PAGE_SIZE>(p, size));
        madvise(p, size, MADV_DONTNEED);
      }
      else
      {
        ::memset(p, 0, size);
      }
    }

    template<bool committed>
    void* reserve(const size_t* size) noexcept
    {
      void* p = mmap(
        nullptr,
        *size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);

      if (p == MAP_FAILED)
        error("Out of memory");

      return p;
    }
  };
} // namespace snmalloc
#endif
