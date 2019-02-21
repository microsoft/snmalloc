#pragma once

namespace snmalloc
{
  void error(const char* const str);

  /**
   * Flags in a bitfield of optional features that a PAL may support.  These
   * should be set in the PAL's `pal_features` static constexpr field.
   */
  enum PalFeatures : uint64_t
  {
    /**
     * This PAL supports low memory notifications.  It must implement a
     * `low_memory_epoch` method that returns a `uint64_t` of the number of
     * times that a low-memory notification has been raised and an
     * `expensive_low_memory_check()` method that returns a `bool` indicating
     * whether low memory conditions are still in effect.
     */
    LowMemoryNotification = (1 << 0)
  };

}

// If simultating OE, then we need the underlying platform
#if !defined(OPEN_ENCLAVE) || defined(OPEN_ENCLAVE_SIMULATION)
#  include "pal_free_bsd_kernel.h"
#  include "pal_freebsd.h"
#  include "pal_linux.h"
#  include "pal_windows.h"
#endif
#include "pal_open_enclave.h"
#include "pal_plain.h"

namespace snmalloc
{
#if !defined(OPEN_ENCLAVE) || defined(OPEN_ENCLAVE_SIMULATION)
  using DefaultPal =
#  if defined(_WIN32)
    PALWindows;
#  elif defined(__linux__)
    PALLinux;
#  elif defined(FreeBSD_KERNEL)
    PALFreeBSDKernel;
#  elif defined(__FreeBSD__)
    PALFBSD;
#  endif
#endif

  using Pal =
#ifdef OPEN_ENCLAVE
    PALPlainMixin<PALOpenEnclave>;
#elif defined(SNMALLOC_MEMORY_PROVIDER)
    PALPlainMixin<SNMALLOC_MEMORY_PROVIDER>;
#else
    DefaultPal;
#endif

  inline void error(const char* const str)
  {
    Pal::error(str);
  }
}
