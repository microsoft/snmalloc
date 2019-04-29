#pragma once

#include "pal_consts.h"

namespace snmalloc
{
  void error(const char* const str);
} // namespace snmalloc

// If simultating OE, then we need the underlying platform
#if !defined(OPEN_ENCLAVE) || defined(OPEN_ENCLAVE_SIMULATION)
#  include "pal_apple.h"
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
#  elif defined(__APPLE__)
    PALApple;
#  elif defined(__linux__)
    PALLinux;
#  elif defined(FreeBSD_KERNEL)
    PALFreeBSDKernel;
#  elif defined(__FreeBSD__)
    PALFBSD;
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
} // namespace snmalloc
