#pragma once

#include "../ds/bits.h"
#include "../mem/allocconfig.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
// VirtualAlloc2 is exposed in RS5 headers.
#  ifdef NTDDI_WIN10_RS5
#    if (NTDDI_VERSION >= NTDDI_WIN10_RS5) && \
      (WINVER >= _WIN32_WINNT_WIN10) && !defined(USE_SYSTEMATIC_TESTING)
#      define PLATFORM_HAS_VIRTUALALLOC2
#    endif
#  endif

namespace snmalloc
{
  class PALWindows
  {
    /**
     * The number of times that the memory pressure notification has fired.
     */
    static std::atomic<uint64_t> pressure_epoch;
    /**
     * A flag indicating that we have tried to register for low-memory
     * notifications.
     */
    static std::atomic<bool> registered_for_notifications;
    static HANDLE lowMemoryObject;
    /**
     * Callback, used when the system delivers a low-memory notification.  This
     * simply increments an atomic counter each time the notification is raised.
     */
    static void CALLBACK low_memory(_In_ PVOID, _In_ BOOLEAN)
    {
      pressure_epoch++;
    }

  public:
    PALWindows()
    {
      // No error handling here - if this doesn't work, then we will just
      // consume more memory.  There's nothing sensible that we could do in
      // error handling.  We also leak both the low memory notification object
      // handle and the wait object handle.  We'll need them until the program
      // exits, so there's little point doing anything else.
      //
      // We only try to register once.  If this fails, give up.  Even if we
      // create multiple PAL objects, we don't want to get more than one
      // callback.
      if (!registered_for_notifications.exchange(true))
      {
        lowMemoryObject =
          CreateMemoryResourceNotification(LowMemoryResourceNotification);
        HANDLE waitObject;
        RegisterWaitForSingleObject(
          &waitObject,
          lowMemoryObject,
          low_memory,
          nullptr,
          INFINITE,
          WT_EXECUTEDEFAULT);
      }
    }

    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.  This PAL supports low-memory notifications.
     */
    static constexpr uint64_t pal_features = LowMemoryNotification
#  if defined(PLATFORM_HAS_VIRTUALALLOC2) || defined(USE_SYSTEMATIC_TESTING)
      | AlignedAllocation
#  endif
      ;

    /**
     * Counter values for the number of times that a low-pressure notification
     * has been delivered.  Callers should compare this with a previous value
     * to see if the low memory state has been triggered since they last
     * checked.
     */
    uint64_t low_memory_epoch()
    {
      return pressure_epoch.load(std::memory_order_acquire);
    }

    /**
     * Check whether the low memory state is still in effect.  This is an
     * expensive operation and should not be on any fast paths.
     */
    bool expensive_low_memory_check()
    {
      BOOL result;
      QueryMemoryResourceNotification(lowMemoryObject, &result);
      return result;
    }

    static void error(const char* const str)
    {
      puts(str);
      fflush(stdout);
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

    /// Notify platform that we will be using these pages
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
    template<bool committed>
    void* reserve(size_t* size, size_t align) noexcept
    {
      DWORD flags = MEM_RESERVE;

      if (committed)
        flags |= MEM_COMMIT;

      size_t retries = 1000;
      void* p;
      size_t request = *size;

      do
      {
        p = VirtualAlloc(
          (void*)systematic_bump_ptr(), request, flags, PAGE_READWRITE);

        systematic_bump_ptr() += request;
        retries--;
      } while (p == nullptr && retries > 0);

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
#  elif defined(PLATFORM_HAS_VIRTUALALLOC2)
    template<bool committed>
    void* reserve(size_t* size, size_t align) noexcept
    {
      DWORD flags = MEM_RESERVE;

      if (committed)
        flags |= MEM_COMMIT;

      // Windows doesn't let you request memory less than 64KB aligned.  Most
      // operating systems will simply give you something more aligned than you
      // ask for, but Windows complains about invalid parameters.
      const size_t min_align = 64 * 1024;
      if (align < min_align)
        align = min_align;

      // If we're on Windows 10 or newer, we can use the VirtualAlloc2
      // function.  The FromApp variant is useable by UWP applications and
      // cannot allocate executable memory.
      MEM_ADDRESS_REQUIREMENTS addressReqs = {0};
      MEM_EXTENDED_PARAMETER param = {0};
      addressReqs.Alignment = align;
      param.Type = MemExtendedParameterAddressRequirements;
      param.Pointer = &addressReqs;
      void* ret = VirtualAlloc2FromApp(
        nullptr, nullptr, *size, flags, PAGE_READWRITE, &param, 1);
      if (ret == nullptr)
      {
        error("Failed to allocate memory\n");
      }
      return ret;
    }
#  else
    template<bool committed>
    void* reserve(size_t* size) noexcept
    {
      DWORD flags = MEM_RESERVE;

      if (committed)
        flags |= MEM_COMMIT;

      void* ret = VirtualAlloc(nullptr, *size, flags, PAGE_READWRITE);
      if (ret == nullptr)
      {
        error("Failed to allocate memory\n");
      }
      return ret;
    }
#  endif
  };
  HEADER_GLOBAL std::atomic<uint64_t> PALWindows::pressure_epoch;
  HEADER_GLOBAL std::atomic<bool> PALWindows::registered_for_notifications;
  HEADER_GLOBAL HANDLE PALWindows::lowMemoryObject;
}
#endif
