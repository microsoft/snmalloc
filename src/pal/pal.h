#pragma once

#include "pal_consts.h"

// If simultating OE, then we need the underlying platform
#if !defined(OPEN_ENCLAVE) || defined(OPEN_ENCLAVE_SIMULATION)
#  include "pal_apple.h"
#  include "pal_freebsd.h"
#  include "pal_freebsd_kernel.h"
#  include "pal_linux.h"
#  include "pal_netbsd.h"
#  include "pal_openbsd.h"
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
#  elif defined(__APPLE__)
    PALApple<>;
#  elif defined(__linux__)
    PALLinux;
#  elif defined(FreeBSD_KERNEL)
    PALFreeBSDKernel;
#  elif defined(__FreeBSD__)
    PALFreeBSD;
#  elif defined(__NetBSD__)
    PALNetBSD;
#  elif defined(__OpenBSD__)
    PALOpenBSD;
#  else
#    error Unsupported platform
#  endif
#endif

  using Pal =
#if defined(SNMALLOC_MEMORY_PROVIDER)
    PALPlainMixin<SNMALLOC_MEMORY_PROVIDER>;
#elif defined(OPEN_ENCLAVE)
    PALPlainMixin<PALOpenEnclave>;
#else
    DefaultPal;
#endif

  inline void error(const char* const str)
  {
    Pal::error(str);
  }

  /**
   * Query whether the PAL supports a specific feature.
   */
  template<PalFeatures F, typename PAL = Pal>
  constexpr static bool pal_supports = (PAL::pal_features & F) == F;
} // namespace snmalloc
