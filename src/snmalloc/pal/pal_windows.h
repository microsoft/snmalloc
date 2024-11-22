#pragma once

#include "../aal/aal.h"
#include "pal_tid_default.h"
#include "pal_timer_default.h"

#ifdef _WIN32
#  ifndef _MSC_VER
#    include <cstdio>
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

#  include <chrono>

namespace snmalloc
{
  class PALWindows : public PalTimerDefaultImpl<PALWindows>,
                     public PalTidDefault
  {
    /**
     * A flag indicating that we have tried to register for low-memory
     * notifications.
     */
    static inline std::atomic<bool> registered_for_notifications;
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

  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.  This PAL supports low-memory notifications.
     */
    static constexpr uint64_t pal_features = LowMemoryNotification | Entropy |
      Time
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
      return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
    }

#  ifdef PLATFORM_HAS_WAITONADDRESS
    using WaitingWord = char;

    template<class T>
    static void wait_on_address(std::atomic<T>& addr, T expected)
    {
      while (addr.load(std::memory_order_relaxed) == expected)
      {
        if (::WaitOnAddress(&addr, &expected, sizeof(T), INFINITE))
          break;
      }
    }

    template<class T>
    static void notify_one_on_address(std::atomic<T>& addr)
    {
      ::WakeByAddressSingle(&addr);
    }

    template<class T>
    static void notify_all_on_address(std::atomic<T>& addr)
    {
      ::WakeByAddressAll(&addr);
    }
#  endif
  };
}
#endif
