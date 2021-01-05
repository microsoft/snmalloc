#pragma once
#include "../ds/defines.h"

#include <chrono>
#include <cstdint>

#if defined(__i386__) || defined(_M_IX86) || defined(_X86_) || \
  defined(__amd64__) || defined(__x86_64__) || defined(_M_X64) || \
  defined(_M_AMD64)
#  if defined(SNMALLOC_SGX)
#    define PLATFORM_IS_X86_SGX
#    define SNMALLOC_NO_AAL_BUILTINS
#  else
#    define PLATFORM_IS_X86
#  endif
#endif

#if defined(__arm__) || defined(__aarch64__)
#  define PLATFORM_IS_ARM
#endif

#if defined(__powerpc__) || defined(__powerpc64__)
#  define PLATFORM_IS_POWERPC
#endif

#if defined(__sparc__)
#  define PLATFORM_IS_SPARC
#endif

namespace snmalloc
{
  /**
   * Flags in a bitfield of attributes of this architecture, much like
   * PalFeatures.
   */
  enum AalFeatures : uint64_t
  {
    /**
     * This architecture does not discriminate between integers and pointers,
     * and so may use bit operations on pointer values.
     */
    IntegerPointers = (1 << 0),
    /**
     * This architecture cannot access cpu cycles counters.
     */
    NoCpuCycleCounters = (1 << 1),
    /**
     * This architecture enforces strict pointer provenance; we bound the
     * pointers given out on malloc() and friends and must, therefore retain
     * internal high-privilege pointers for recycling memory on free().
     */
    StrictProvenance = (1 << 2),
  };

  enum AalName : int
  {
    ARM,
    PowerPC,
    X86,
    X86_SGX,
    Sparc,
  };

  /**
   * Architecture Abstraction Layer. Includes default implementations of some
   * functions using compiler builtins.  Falls back to the definitions in the
   * platform's AAL if the builtin does not exist.
   */
  template<class Arch>
  struct AAL_Generic : Arch
  {
    /*
     * Provide a default specification of address_t as uintptr_t for Arch-es
     * that support IntegerPointers.  Those Arch-es without IntegerPoihnters
     * must explicitly give their address_t.
     *
     * This somewhat obtuse way of spelling the defaulting is necessary so
     * that all arguments to std::conditional_t are valid, even if they
     * wouldn't be valid in context.  One might rather wish to say
     *
     *   std::conditional_t<..., uintptr_t, Arch::address_t>
     *
     * but that requires that Arch::address_t always be given, precisely
     * the thing we're trying to avoid with the conditional.
     */

    struct default_address_t
    {
      using address_t = uintptr_t;
    };

    using address_t = typename std::conditional_t<
      (Arch::aal_features & IntegerPointers) != 0,
      default_address_t,
      Arch>::address_t;

    /**
     * Prefetch a specific address.
     *
     * If the compiler provides a portable prefetch builtin, use it directly,
     * otherwise delegate to the architecture-specific layer.  This allows new
     * architectures to avoid needing to implement a custom `prefetch` method
     * if they are used only with a compiler that provides the builtin.
     */
    static inline void prefetch(void* ptr)
    {
#if __has_builtin(__builtin_prefetch) && !defined(SNMALLOC_NO_AAL_BUILTINS)
      __builtin_prefetch(ptr);
#else
      Arch::prefetch(ptr);
#endif
    }

    /**
     * Return an architecture-specific cycle counter.
     *
     * If the compiler provides a portable prefetch builtin, use it directly,
     * otherwise delegate to the architecture-specific layer.  This allows new
     * architectures to avoid needing to implement a custom `tick` method
     * if they are used only with a compiler that provides the builtin.
     */
    static inline uint64_t tick()
    {
      if constexpr (
        (Arch::aal_features & NoCpuCycleCounters) == NoCpuCycleCounters)
      {
        auto tick = std::chrono::high_resolution_clock::now();
        return static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
            tick.time_since_epoch())
            .count());
      }
      else
      {
#if __has_builtin(__builtin_readcyclecounter) && \
  !defined(SNMALLOC_NO_AAL_BUILTINS)
        return __builtin_readcyclecounter();
#else
        return Arch::tick();
#endif
      }
    }
  };

} // namespace snmalloc

#if defined(PLATFORM_IS_X86)
#  include "aal_x86.h"
#elif defined(PLATFORM_IS_X86_SGX)
#  include "aal_x86_sgx.h"
#elif defined(PLATFORM_IS_ARM)
#  include "aal_arm.h"
#elif defined(PLATFORM_IS_POWERPC)
#  include "aal_powerpc.h"
#elif defined(PLATFORM_IS_SPARC)
#  include "aal_sparc.h"
#endif

namespace snmalloc
{
  using Aal = AAL_Generic<AAL_Arch>;

  template<AalFeatures F, typename AAL = Aal>
  constexpr static bool aal_supports = (AAL::aal_features & F) == F;
} // namespace snmalloc

#if defined(_MSC_VER) && defined(SNMALLOC_VA_BITS_32)
#  include <intsafe.h>
#endif

#ifdef __POINTER_WIDTH__
#  if ((__POINTER_WIDTH__ == 64) && !defined(SNMALLOC_VA_BITS_64)) || \
    ((__POINTER_WIDTH__ == 32) && !defined(SNMALLOC_VA_BITS_32))
#    error Compiler and PAL define inconsistent bit widths
#  endif
#endif

#if defined(SNMALLOC_VA_BITS_32) && defined(SNMALLOC_VA_BITS_64)
#  error Only one of SNMALLOC_VA_BITS_64 and SNMALLOC_VA_BITS_32 may be defined!
#endif

#ifdef SNMALLOC_VA_BITS_32
static_assert(sizeof(size_t) == 4);
#elif defined(SNMALLOC_VA_BITS_64)
static_assert(sizeof(size_t) == 8);
#endif
