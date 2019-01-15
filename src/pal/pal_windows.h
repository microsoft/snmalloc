#pragma once

#include "../ds/bits.h"
#include "../mem/allocconfig.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>

namespace snmalloc
{
  class PALWindows
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

      BOOL ok = VirtualFree(p, size, MEM_DECOMMIT);

      if (!ok)
        error("VirtualFree failed");
    }

    /// Notify platform that we will not be using these pages
    template<ZeroMem zero_mem>
    void notify_using(void* p, size_t size) noexcept
    {
      assert(
        bits::is_aligned_block<OS_PAGE_SIZE>(p, size) || (zero_mem == NoZero));

      void* r = VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE);

      if (r == nullptr)
        error("out of memory");
    }

    /// OS specific function for zeroing memory
    template<bool page_aligned = false>
    void zero(void* p, size_t size) noexcept
    {
      if (page_aligned || bits::is_aligned_block<OS_PAGE_SIZE>(p, size))
      {
        assert(bits::is_aligned_block<OS_PAGE_SIZE>(p, size));
        notify_not_using(p, size);
        notify_using<YesZero>(p, size);
      }
      else
        ::memset(p, 0, size);
    }

#  ifdef USE_SYSTEMATIC_TESTING
    size_t& systematic_bump_ptr()
    {
      static size_t bump_ptr = (size_t)0x4000'0000'0000;
      return bump_ptr;
    }
#  endif

    template<bool committed>
    void* reserve(size_t* size, size_t align) noexcept
    {
      // Add align, so we can guarantee to provide at least size.
      size_t request = *size + align;

      // Alignment must be a power of 2.
      assert(align == bits::next_pow2(align));

      DWORD flags = MEM_RESERVE;

      if (committed)
        flags |= MEM_COMMIT;

      void* p;
#  ifdef USE_SYSTEMATIC_TESTING
      size_t retries = 1000;
      do
      {
        p = VirtualAlloc(
          (void*)systematic_bump_ptr(), request, flags, PAGE_READWRITE);

        systematic_bump_ptr() += request;
        retries--;
      } while (p == nullptr && retries > 0);
#  else
      p = VirtualAlloc(nullptr, request, flags, PAGE_READWRITE);
#  endif

      uintptr_t aligned_p = bits::align_up((size_t)p, align);

      if (aligned_p != (uintptr_t)p)
      {
        auto extra_bit = aligned_p - (uintptr_t)p;
        uintptr_t end = (uintptr_t)p + request;
        // Attempt to align end of the block.
        VirtualAlloc((void*)end, extra_bit, flags, PAGE_READWRITE);
      }
      *size = request;
      return p;
    }
  };
}
#endif