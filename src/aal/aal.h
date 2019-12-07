#pragma once
#include "../ds/defines.h"

#include <cstdint>

#if defined(__i386__) || defined(_M_IX86) || defined(_X86_) || \
  defined(__amd64__) || defined(__x86_64__) || defined(_M_X64) || \
  defined(_M_AMD64)
#  define PLATFORM_IS_X86
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
  };

  /**
   * Architecture Abstraction Layer. Includes default implementations of some
   * functions using compiler builtins.  Falls back to the definitions in the
   * platform's AAL if the builtin does not exist.
   */
  template<class Arch>
  struct AAL_Generic : Arch
  {
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
#if __has_builtin(__builtin_readcyclecounter) && \
  !defined(SNMALLOC_NO_AAL_BUILTINS)
      return __builtin_readcyclecounter();
#else
      return Arch::tick();
#endif
    }
  };

} // namespace snmalloc

#ifdef PLATFORM_IS_X86
#  include "aal_x86.h"
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
