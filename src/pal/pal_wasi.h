#pragma once

#include "ds/address.h"
#include "ds/flaglock.h"
#include "pal_plain.h"

#ifdef WASM_ENV //wasi-libc/libc-bottom-half/headers/public/__header_*

#include <stdio.h>
#include <unistd.h>
#include <array>

extern "C" void *memset(void *dst, int c, size_t n);
extern "C" [[noreturn]] void w_abort();

// this pal uses wasi libc bottom half & wasm linear memory (wlm)
namespace snmalloc
{
  class PALWASI
  {
    // This is infrequently used code, a spin lock simplifies the code
    // considerably, and should never be on the fast path.
    static inline std::atomic_flag spin_lock;

  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     */
    static constexpr uint64_t pal_features = 0;

    static constexpr size_t page_size = 0x10000;

    [[noreturn]] static void error(const char* const str)
    {
      fprintf(stderr, "%s\n", str);
      abort();
    }

    static std::pair<void*, size_t>
    reserve_at_least(size_t request_size) noexcept
    {
      FlagLock lock(spin_lock);
      intptr_t actual_size = ((request_size + PAGESIZE - 1) / PAGESIZE) * PAGESIZE;
      void *start = sbrk(actual_size);
      if (start == (void*)-1)
        return {nullptr, 0};

      return std::make_pair(start, actual_size);
    }

    template<bool page_aligned = false>
    void zero(void* p, size_t size) noexcept
    {
      memset(p, 0, size);
    }
  };
}

// WASI does not support pthreads and thus neither can it offer __cxa_thread_atexit.
// Delegating to __cxa_atexit.
// Should be changed once pthreads support it live and desired.
extern "C"
{
  int __cxa_atexit(void (*func) (void*), void* arg, void* dso_handle);
  int __cxa_thread_atexit(void (*func) (void*), void* arg, void* dso_symbol)
  {
    return __cxa_atexit(func, arg, dso_symbol);
  }
}

#endif
