#pragma once

#include "../ds/concept.h"
#include "pal_concept.h"
#include "pal_consts.h"

// If simultating OE, then we need the underlying platform
#if defined(OPEN_ENCLAVE)
#  include "pal_open_enclave.h"
#endif
#if !defined(OPEN_ENCLAVE) || defined(OPEN_ENCLAVE_SIMULATION)
#  include "pal_apple.h"
#  include "pal_dragonfly.h"
#  include "pal_freebsd.h"
#  include "pal_freebsd_kernel.h"
#  include "pal_haiku.h"
#  include "pal_linux.h"
#  include "pal_netbsd.h"
#  include "pal_noalloc.h"
#  include "pal_openbsd.h"
#  include "pal_solaris.h"
#  include "pal_windows.h"
#endif
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
#  elif defined(__HAIKU__)
    PALHaiku;
#  elif defined(__NetBSD__)
    PALNetBSD;
#  elif defined(__OpenBSD__)
    PALOpenBSD;
#  elif defined(__sun)
    PALSolaris;
#  elif defined(__DragonFly__)
    PALDragonfly;
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

  [[noreturn]] SNMALLOC_SLOW_PATH inline SNMALLOC_COLD void
  error(const char* const str)
  {
    Pal::error(str);
  }

  // Used to keep Superslab metadata committed.
  static constexpr size_t OS_PAGE_SIZE = Pal::page_size;

  /**
   * Perform platform-specific adjustment of return pointers.
   *
   * This is here, rather than in every PAL proper, merely to minimize
   * disruption to PALs for platforms that do not support StrictProvenance AALs.
   */
  template<typename PAL = Pal, typename AAL = Aal, typename T, capptr_bounds B>
  static SNMALLOC_FAST_PATH typename std::enable_if_t<
    !aal_supports<StrictProvenance, AAL>,
    CapPtr<T, capptr_export_type<B>()>>
  capptr_export(CapPtr<T, B> p)
  {
    return CapPtr<T, capptr_export_type<B>()>(p.unsafe_capptr);
  }

  template<typename PAL = Pal, typename AAL = Aal, typename T, capptr_bounds B>
  static SNMALLOC_FAST_PATH typename std::enable_if_t<
    aal_supports<StrictProvenance, AAL>,
    CapPtr<T, capptr_export_type<B>()>>
  capptr_export(CapPtr<T, B> p)
  {
    return PAL::capptr_export(p);
  }

  /**
   * A convenience wrapper that avoids the need to litter unsafe accesses with
   * every call to PAL::zero.
   *
   * We do this here rather than plumb CapPtr further just to minimize
   * disruption and avoid code bloat.  This wrapper ought to compile down to
   * nothing if SROA is doing its job.
   */
  template<typename PAL, bool page_aligned = false, typename T, capptr_bounds B>
  static SNMALLOC_FAST_PATH void pal_zero(CapPtr<T, B> p, size_t sz)
  {
    static_assert(
      !page_aligned || B == CBArena || B == CBChunkD || B == CBChunk);
    PAL::template zero<page_aligned>(p.unsafe_capptr, sz);
  }

  static_assert(
    bits::is_pow2(OS_PAGE_SIZE), "OS_PAGE_SIZE must be a power of two");
  static_assert(
    OS_PAGE_SIZE % Aal::smallest_page_size == 0,
    "The smallest architectural page size must divide OS_PAGE_SIZE");

  // Some system headers (e.g. Linux' sys/user.h, FreeBSD's machine/param.h)
  // define `PAGE_SIZE` as a macro.  We don't use `PAGE_SIZE` as our variable
  // name, to avoid conflicts, but if we do see a macro definition then check
  // that our value matches the platform's expected value.
  // On macOS 11, system headers (mach/i386/vm_param.h and mach/arm/vm_param.h)
  // define `PAGE_SIZE` as an extern making it unsuitable for this assertion.
#if !defined(__APPLE__) && defined(PAGE_SIZE)
  static_assert(
    PAGE_SIZE == OS_PAGE_SIZE,
    "Page size from system header does not match snmalloc config page size.");
#endif

} // namespace snmalloc
