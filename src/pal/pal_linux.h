#pragma once

#if defined(__linux__)
#  include "../ds/bits.h"
#  include "../mem/allocconfig.h"

#  include <stdio.h>
#  include <string.h>
#  include <sys/mman.h>

namespace snmalloc
{
  class PALLinux
  {
  public:
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

    /// Notify platform that we will not be using these pages
    template<ZeroMem zero_mem>
    void notify_using(void* p, size_t size) noexcept
    {
      assert(
        bits::is_aligned_block<OS_PAGE_SIZE>(p, size) || (zero_mem == NoZero));

      if (zero_mem == YesZero)
        zero<true>(p, size);
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
    void* reserve(size_t* size, size_t align) noexcept
    {
      size_t request = *size;
      // Add align, so we can guarantee to provide at least size.
      request += align;
      // Alignment must be a power of 2.
      assert(align == bits::next_pow2(align));

      void* p = mmap(
        NULL,
        request,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);

      if (p == MAP_FAILED)
        error("Out of memory");
      *size = request;
      uintptr_t p0 = (uintptr_t)p;
      uintptr_t start = bits::align_up(p0, align);

      if (start > (uintptr_t)p0)
      {
        uintptr_t end = bits::align_down(p0 + request, align);
        *size = end - start;
        munmap(p, start - p0);
        munmap((void*)end, (p0 + request) - end);
        p = (void*)start;
      }
      return p;
    }
  };
}
#endif
