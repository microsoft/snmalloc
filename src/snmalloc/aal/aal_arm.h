#pragma once

#if defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
#  define SNMALLOC_VA_BITS_64
#  ifdef _MSC_VER
#    include <arm64_neon.h>
#  endif
#else
#  define SNMALLOC_VA_BITS_32
#  ifdef _MSC_VER
#    include <arm_neon.h>
#  endif
#endif

#include <stddef.h>
#include <stdint.h>

#if defined(SNMALLOC_COMPILER_SUPPORT_PACA_PACG) && \
  defined(__ARM_FEATURE_PAUTH) && __has_include(<ptrauth.h>)
#  include <ptrauth.h>
#endif

namespace snmalloc
{
  /**
   * ARM-specific architecture abstraction layer.
   */
  class AAL_arm
  {
  public:
    /**
     * Bitmap of AalFeature flags
     */
    static constexpr uint64_t aal_features = IntegerPointers
#if defined(SNMALLOC_VA_BITS_32) || !defined(__APPLE__)
      | NoCpuCycleCounters
#endif
#if defined(SNMALLOC_COMPILER_SUPPORT_PACA_PACG) && \
  defined(__ARM_FEATURE_PAUTH) && __has_include(<ptrauth.h>)
      | PtrAuthentication
#endif
      ;

    static constexpr enum AalName aal_name = ARM;

    static constexpr size_t smallest_page_size = 0x1000;

    /**
     * On pipelined processors, notify the core that we are in a spin loop and
     * that speculative execution past this point may not be a performance gain.
     */
    static inline void pause()
    {
#ifdef _MSC_VER
      __yield();
#else
      __asm__ volatile("yield");
#endif
    }

    static inline void prefetch(void* ptr)
    {
#ifdef _MSC_VER
      __prefetch(ptr);
#elif __has_builtin(__builtin_prefetch) && !defined(SNMALLOC_NO_AAL_BUILTINS)
      __builtin_prefetch(ptr);
#elif defined(SNMALLOC_VA_BITS_64)
      __asm__ volatile("prfm pstl1keep, [%0]" : "=r"(ptr));
#else
      __asm__ volatile("pld\t[%0]" : "=r"(ptr));
#endif
    }

#if defined(SNMALLOC_VA_BITS_64) && defined(__APPLE__)
    static inline uint64_t tick() noexcept
    {
      uint64_t t;
      __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
      return t;
    }
#endif

#if defined(SNMALLOC_COMPILER_SUPPORT_PACA_PACG) && \
  defined(__ARM_FEATURE_PAUTH) && __has_include(<ptrauth.h>)
    static SNMALLOC_FAST_PATH uintptr_t pointer_auth_sign_data(
      uintptr_t value, uintptr_t storage_addr, uintptr_t tweak) noexcept
    {
      return unsafe_to_uintptr<void>(ptrauth_sign_unauthenticated(
        unsafe_from_uintptr<void>(value),
        ptrauth_key_process_dependent_data,
        /*
         * only the lower 16 bits of tweak is used
         */
        ptrauth_blend_discriminator(
          unsafe_from_uintptr<void>(storage_addr), tweak)));
    }

    static SNMALLOC_FAST_PATH uintptr_t pointer_auth_auth_data(
      uintptr_t value, uintptr_t storage_addr, uintptr_t tweak) noexcept
    {
      return unsafe_to_uintptr<void>(ptrauth_auth_data(
        unsafe_from_uintptr<void>(value),
        ptrauth_key_process_dependent_data,
        /*
         * only the lower 16 bits of tweak is used
         */
        ptrauth_blend_discriminator(
          unsafe_from_uintptr<void>(storage_addr), tweak)));
    }
#endif
  };

  using AAL_Arch = AAL_arm;
} // namespace snmalloc
