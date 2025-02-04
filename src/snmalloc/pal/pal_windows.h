#pragma once

#include "../aal/aal.h"
#include "../ds_aal/ds_aal.h"
#include "pal_timer_default.h"

#ifdef _WIN32
#  ifndef _MSC_VER
#    include <errno.h>
#    include <stdio.h>
#  endif
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  pragma comment(lib, "bcrypt.lib")
#  include <bcrypt.h>
// VirtualAlloc2 is exposed in RS5 headers.
#  ifdef NTDDI_WIN10_RS5
#    if (NTDDI_VERSION >= NTDDI_WIN10_RS5) && \
      (WINVER >= _WIN32_WINNT_WIN10) && !defined(USE_SYSTEMATIC_TESTING)
#      define PLATFORM_HAS_VIRTUALALLOC2
#      define PLATFORM_HAS_WAITONADDRESS
#    endif
#  endif

namespace snmalloc
{
  class PALWindows : public PalTimerDefaultImpl<PALWindows>
  {
    /**
     * A flag indicating that we have tried to register for low-memory
     * notifications.
     */
    static inline stl::Atomic<bool> registered_for_notifications;
    static inline HANDLE lowMemoryObject;

    /**
     * List of callbacks for low-memory notification
     */
    static inline PalNotifier low_memory_callbacks;

    /**
     * Callback, used when the system delivers a low-memory notification.  This
     * calls all the handlers registered with the PAL.
     */
    static void CALLBACK low_memory(_In_ PVOID, _In_ BOOLEAN)
    {
      low_memory_callbacks.notify_all();
    }

    // A list of reserved ranges, used to handle lazy commit on readonly pages.
    // We currently only need one, so haven't implemented a backup if the
    // initial 16 is insufficient.
    inline static stl::Array<stl::Pair<address_t, size_t>, 16> reserved_ranges;

    // Lock for the reserved ranges.
    inline static FlagWord reserved_ranges_lock{};

    // Exception handler for handling lazy commit on readonly pages.
    static LONG NTAPI
    HandleReadonlyLazyCommit(struct _EXCEPTION_POINTERS* ExceptionInfo)
    {
      // Check this is an AV
      if (
        ExceptionInfo->ExceptionRecord->ExceptionCode !=
        EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

      // Check this is a read access
      if (ExceptionInfo->ExceptionRecord->ExceptionInformation[0] != 0)
        return EXCEPTION_CONTINUE_SEARCH;

      // Get faulting address from exception info.
      snmalloc::address_t faulting_address =
        ExceptionInfo->ExceptionRecord->ExceptionInformation[1];

      bool found = false;
      {
        FlagLock lock(reserved_ranges_lock);
        // Check if the address is in a reserved range.
        for (auto& r : reserved_ranges)
        {
          if ((faulting_address - r.first) < r.second)
          {
            found = true;
            break;
          }
        }
      }

      if (!found)
        return EXCEPTION_CONTINUE_SEARCH;

      // Commit the page as readonly
      auto pagebase = snmalloc::bits::align_down(faulting_address, page_size);
      VirtualAlloc((void*)pagebase, page_size, MEM_COMMIT, PAGE_READONLY);

      // Resume execution
      return EXCEPTION_CONTINUE_EXECUTION;
    }

    static void initialise_for_singleton(size_t*) noexcept
    {
      AddVectoredExceptionHandler(1, HandleReadonlyLazyCommit);
    }

    // Ensure the exception handler is registered.
    static void initialise_readonly_av() noexcept
    {
      static Singleton<size_t, &initialise_for_singleton> init;
      init.get();
    }

  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.  This PAL supports low-memory notifications.
     */
    static constexpr uint64_t pal_features = LowMemoryNotification | Entropy |
      Time | LazyCommit
#  if defined(PLATFORM_HAS_VIRTUALALLOC2) && !defined(USE_SYSTEMATIC_TESTING)
      | AlignedAllocation
#  endif
#  if defined(PLATFORM_HAS_WAITONADDRESS)
      | WaitOnAddress
#  endif
      ;

    static SNMALLOC_CONSTINIT_STATIC size_t minimum_alloc_size = 0x10000;

    static constexpr size_t page_size = 0x1000;

    /**
     * Windows always inherits its underlying architecture's full address range.
     */
    static constexpr size_t address_bits = Aal::address_bits;

    /**
     * Check whether the low memory state is still in effect.  This is an
     * expensive operation and should not be on any fast paths.
     */
    static bool expensive_low_memory_check()
    {
      BOOL result;
      QueryMemoryResourceNotification(lowMemoryObject, &result);
      return result;
    }

    /**
     * Register callback object for low-memory notifications.
     * Client is responsible for allocation, and ensuring the object is live
     * for the duration of the program.
     */
    static void
    register_for_low_memory_callback(PalNotificationObject* callback)
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

      low_memory_callbacks.register_notification(callback);
    }

    static void message(const char* const str)
    {
      fputs(str, stderr);
      fputc('\n', stderr);
      fflush(stderr);
    }

    [[noreturn]] static void error(const char* const str)
    {
      message(str);
      abort();
    }

    /// Notify platform that we will not be using these pages
    static void notify_not_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));

      BOOL ok = VirtualFree(p, size, MEM_DECOMMIT);

      if (!ok)
        error("VirtualFree failed");
    }

    /// Notify platform that we will be using these pages
    template<ZeroMem zero_mem>
    static void notify_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(
        is_aligned_block<page_size>(p, size) || (zero_mem == NoZero));

      void* r = VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE);

      if (r == nullptr)
        report_fatal_error(
          "out of memory: {} ({}) could not be committed", p, size);
    }

    static void notify_using_readonly(void* p, size_t size) noexcept
    {
      initialise_readonly_av();

      {
        FlagLock lock(reserved_ranges_lock);
        for (auto& r : reserved_ranges)
        {
          if (r.first == 0)
          {
            r.first = (address_t)p;
            r.second = size;
            return;
          }
        }
      }

      error("Implementation error: Too many lazy commit regions!");
    }

    /// OS specific function for zeroing memory
    template<bool page_aligned = false>
    static void zero(void* p, size_t size) noexcept
    {
      if (page_aligned || is_aligned_block<page_size>(p, size))
      {
        SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));
        notify_not_using(p, size);
        notify_using<YesZero>(p, size);
      }
      else
        ::memset(p, 0, size);
    }

#  ifdef PLATFORM_HAS_VIRTUALALLOC2
    template<bool state_using>
    static void* reserve_aligned(size_t size) noexcept
    {
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(size >= minimum_alloc_size);

      DWORD flags = MEM_RESERVE;

      if (state_using)
        flags |= MEM_COMMIT;

      // If we're on Windows 10 or newer, we can use the VirtualAlloc2
      // function.  The FromApp variant is useable by UWP applications and
      // cannot allocate executable memory.
      MEM_ADDRESS_REQUIREMENTS addressReqs = {NULL, NULL, size};

      MEM_EXTENDED_PARAMETER param = {
        {MemExtendedParameterAddressRequirements, 0}, {0}};
      // Separate assignment as MSVC doesn't support .Pointer in the
      // initialisation list.
      param.Pointer = &addressReqs;

      void* ret = VirtualAlloc2FromApp(
        nullptr, nullptr, size, flags, PAGE_READWRITE, &param, 1);
      if (ret == nullptr)
        errno = ENOMEM;
      return ret;
    }
#  endif

    static void* reserve(size_t size) noexcept
    {
      void* ret = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE);
      if (ret == nullptr)
        errno = ENOMEM;
      return ret;
    }

    /**
     * Source of Entropy
     */
    static uint64_t get_entropy64()
    {
      uint64_t result;
      if (
        BCryptGenRandom(
          nullptr,
          reinterpret_cast<PUCHAR>(&result),
          sizeof(result),
          BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        error("Failed to get entropy.");
      return result;
    }

    static uint64_t internal_time_in_ms()
    {
      // Performance counter is a high-precision monotonic clock.
      static stl::Atomic<uint64_t> freq_cache = 0;
      constexpr uint64_t ms_per_second = 1000;
      SNMALLOC_UNINITIALISED LARGE_INTEGER buf;
      auto freq = freq_cache.load(stl::memory_order_relaxed);
      if (SNMALLOC_UNLIKELY(freq == 0))
      {
        // On systems that run Windows XP or later, the function will always
        // succeed and will thus never return zero.
        ::QueryPerformanceFrequency(&buf);
        freq = static_cast<uint64_t>(buf.QuadPart);
        freq_cache.store(freq, stl::memory_order_relaxed);
      }
      ::QueryPerformanceCounter(&buf);
      return (static_cast<uint64_t>(buf.QuadPart) * ms_per_second) / freq;
    }

#  ifdef PLATFORM_HAS_WAITONADDRESS
    using WaitingWord = char;

    template<class T>
    static void wait_on_address(stl::Atomic<T>& addr, T expected)
    {
      while (addr.load(stl::memory_order_relaxed) == expected)
      {
        ::WaitOnAddress(&addr, &expected, sizeof(T), INFINITE);
      }
    }

    template<class T>
    static void notify_one_on_address(stl::Atomic<T>& addr)
    {
      ::WakeByAddressSingle(&addr);
    }

    template<class T>
    static void notify_all_on_address(stl::Atomic<T>& addr)
    {
      ::WakeByAddressAll(&addr);
    }
#  endif
  };
}
#endif
