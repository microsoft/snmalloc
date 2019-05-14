#pragma once

#include <limits>
#include <stddef.h>

#ifdef _MSC_VER
#  include <immintrin.h>
#  include <intrin.h>
#  define ALWAYSINLINE __forceinline
#  define NOINLINE __declspec(noinline)
#  define HEADER_GLOBAL __declspec(selectany)
#  define likely(x) !!(x)
#  define unlikely(x) !!(x)
#else
#  define likely(x) __builtin_expect(!!(x), 1)
#  define unlikely(x) __builtin_expect(!!(x), 0)
#  include <cpuid.h>
#  include <emmintrin.h>
#  define ALWAYSINLINE __attribute__((always_inline))
#  define NOINLINE __attribute__((noinline))
#  ifdef __clang__
#    define HEADER_GLOBAL __attribute__((selectany))
#  else
//  GCC does not support selectany, weak is almost the correct
//  attribute, but leaves the global variable preemptible.
#    define HEADER_GLOBAL __attribute__((weak))
#  endif
#endif

#if defined(__i386__) || defined(_M_IX86) || defined(_X86_) || \
  defined(__amd64__) || defined(__x86_64__) || defined(_M_X64) || \
  defined(_M_AMD64)
#  define PLATFORM_IS_X86
#  if defined(__linux__) && !defined(OPEN_ENCLAVE)
#    include <x86intrin.h>
#  endif
#  if defined(__amd64__) || defined(__x86_64__) || defined(_M_X64) || \
    defined(_M_AMD64)
#    define PLATFORM_BITS_64
#  else
#    define PLATFORM_BITS_32
#  endif
#endif

#if defined(_MSC_VER) && defined(PLATFORM_BITS_32)
#  include <intsafe.h>
#endif

#ifndef __has_builtin
#  define __has_builtin(x) 0
#endif

#define UNUSED(x) ((void)(x))

#if __has_builtin(__builtin_assume)
#  define SNMALLOC_ASSUME(x) __builtin_assume(x)
#else
#  define SNMALLOC_ASSUME(x) \
    do \
    { \
    } while (0)
#endif

// #define USE_LZCNT

#include "address.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <type_traits>

#ifdef pause
#  undef pause
#endif

namespace snmalloc
{
  // Used to enable trivial constructors for
  // class that zero init is sufficient.
  // Supplying PreZeroed means the memory is pre-zeroed i.e. a global section
  // RequiresInit is if the class needs to zero its fields.
  enum Construction
  {
    PreZeroed,
    RequiresInit
  };

  namespace bits
  {
    static constexpr size_t BITS = sizeof(size_t) * 8;

    static constexpr bool is64()
    {
      return BITS == 64;
    }

    /**
     * Returns a value of type T that has a single bit set,
     *
     * S is a template parameter because callers use either `int` or `size_t`
     * and either is valid to represent a number in the range 0-63 (or 0-127 if
     * we want to use `__uint128_t` as `T`).
     */
    template<typename T = size_t, typename S>
    constexpr T one_at_bit(S shift)
    {
      static_assert(std::is_integral_v<T>, "Type must be integral");
      return (static_cast<T>(1)) << shift;
    }

    static constexpr size_t ADDRESS_BITS = is64() ? 48 : 32;

    inline void pause()
    {
#if defined(PLATFORM_IS_X86)
      _mm_pause();
#else
#  warning "Missing pause intrinsic"
#endif
    }

    inline uint64_t tick()
    {
#if defined(PLATFORM_IS_X86)
#  if defined(_MSC_VER)
      return __rdtsc();
#  elif defined(__clang__)
      return __builtin_readcyclecounter();
#  else
      return __builtin_ia32_rdtsc();
#  endif
#else
#  error Define CPU tick for this platform
#endif
    }

    inline uint64_t tickp()
    {
#if defined(PLATFORM_IS_X86)
#  if defined(_MSC_VER)
      unsigned int aux;
      return __rdtscp(&aux);
#  else
      unsigned aux;
      return __builtin_ia32_rdtscp(&aux);
#  endif
#else
#  error Define CPU tick for this platform
#endif
    }

    inline void halt_out_of_order()
    {
#if defined(PLATFORM_IS_X86)
#  if defined(_MSC_VER)
      int cpu_info[4];
      __cpuid(cpu_info, 0);
#  else
      unsigned int eax, ebx, ecx, edx;
      __get_cpuid(0, &eax, &ebx, &ecx, &edx);
#  endif
#else
#  error Define CPU benchmark start time for this platform
#endif
    }

    inline uint64_t benchmark_time_start()
    {
      halt_out_of_order();
      return tick();
    }

    inline uint64_t benchmark_time_end()
    {
      uint64_t t = tickp();
      halt_out_of_order();
      return t;
    }

    inline size_t clz(size_t x)
    {
#if defined(_MSC_VER)
#  ifdef USE_LZCNT
#    ifdef PLATFORM_BITS_64
      return __lzcnt64(x);
#    else
      return __lzcnt((uint32_t)x);
#    endif
#  else
      unsigned long index;

#    ifdef PLATFORM_BITS_64
      _BitScanReverse64(&index, x);
#    else
      _BitScanReverse(&index, (unsigned long)x);
#    endif

      return BITS - index - 1;
#  endif
#else
      return static_cast<size_t>(__builtin_clzl(x));
#endif
    }

    inline constexpr size_t rotr_const(size_t x, size_t n)
    {
      size_t nn = n & (BITS - 1);
      return (x >> nn) |
        (x << ((static_cast<size_t>(-static_cast<int>(nn))) & (BITS - 1)));
    }

    inline constexpr size_t rotl_const(size_t x, size_t n)
    {
      size_t nn = n & (BITS - 1);
      return (x << nn) |
        (x >> ((static_cast<size_t>(-static_cast<int>(nn))) & (BITS - 1)));
    }

    inline size_t rotr(size_t x, size_t n)
    {
#if defined(_MSC_VER)
#  ifdef PLATFORM_BITS_64
      return _rotr64(x, (int)n);
#  else
      return _rotr((uint32_t)x, (int)n);
#  endif
#else
      return rotr_const(x, n);
#endif
    }

    inline size_t rotl(size_t x, size_t n)
    {
#if defined(_MSC_VER)
#  ifdef PLATFORM_BITS_64
      return _rotl64(x, (int)n);
#  else
      return _rotl((uint32_t)x, (int)n);
#  endif
#else
      return rotl_const(x, n);
#endif
    }

    constexpr size_t clz_const(size_t x)
    {
      size_t n = 0;

      for (int i = BITS - 1; i >= 0; i--)
      {
        size_t mask = one_at_bit(i);

        if ((x & mask) == mask)
          return n;

        n++;
      }

      return n;
    }

    inline size_t ctz(size_t x)
    {
#if defined(_MSC_VER)
#  ifdef PLATFORM_BITS_64
      return _tzcnt_u64(x);
#  else
      return _tzcnt_u32((uint32_t)x);
#  endif
#else
      return static_cast<size_t>(__builtin_ctzl(x));
#endif
    }

    constexpr size_t ctz_const(size_t x)
    {
      size_t n = 0;

      for (size_t i = 0; i < BITS; i++)
      {
        size_t mask = one_at_bit(i);

        if ((x & mask) == mask)
          return n;

        n++;
      }

      return n;
    }

    inline size_t umul(size_t x, size_t y, bool& overflow)
    {
#if __has_builtin(__builtin_mul_overflow)
      size_t prod;
      overflow = __builtin_mul_overflow(x, y, &prod);
      return prod;
#elif defined(_MSC_VER)
#  if defined(PLATFORM_BITS_64)
      size_t high_prod;
      size_t prod = _umul128(x, y, &high_prod);
      overflow = high_prod != 0;
      return prod;
#  else
      size_t prod;
      overflow = S_OK != UIntMult(x, y, &prod);
      return prod;
#  endif
#else
      size_t prod = x * y;
      overflow = y && (x > ((size_t)-1 / y));
      return prod;
#endif
    }

    inline size_t next_pow2(size_t x)
    {
      // Correct for numbers [0..MAX_SIZE >> 1).
      // Returns 1 for x > (MAX_SIZE >> 1).
      if (x <= 2)
        return x;

      return one_at_bit(BITS - clz(x - 1));
    }

    inline size_t next_pow2_bits(size_t x)
    {
      // Correct for numbers [1..MAX_SIZE].
      // Returns 64 for 0. Approximately 2 cycles.
      return BITS - clz(x - 1);
    }

    constexpr size_t next_pow2_const(size_t x)
    {
      if (x <= 2)
        return x;

      return one_at_bit(BITS - clz_const(x - 1));
    }

    constexpr size_t next_pow2_bits_const(size_t x)
    {
      return BITS - clz_const(x - 1);
    }

    static inline size_t align_down(size_t value, size_t alignment)
    {
      assert(next_pow2(alignment) == alignment);

      size_t align_1 = alignment - 1;
      value &= ~align_1;
      return value;
    }

    static inline size_t align_up(size_t value, size_t alignment)
    {
      assert(next_pow2(alignment) == alignment);

      size_t align_1 = alignment - 1;
      value += align_1;
      value &= ~align_1;
      return value;
    }

    template<size_t alignment>
    static inline bool is_aligned_block(void* p, size_t size)
    {
      assert(next_pow2(alignment) == alignment);

      return ((static_cast<size_t>(address_cast(p)) | size) &
              (alignment - 1)) == 0;
    }

    /************************************************
     *
     * Map large range of strictly positive integers
     * into an exponent and mantissa pair.
     *
     * The reverse mapping is given by first adding one to the value, and then
     * extracting the bottom MANTISSA bits as m, and the rest as e.
     * Then each value maps as:
     *
     *  e |     m      |    value
     * ---------------------------------
     *  0 | x1 ... xm  | 0..00 x1 .. xm
     *  1 | x1 ... xm  | 0..01 x1 .. xm
     *  2 | x1 ... xm  | 0..1 x1 .. xm 0
     *  3 | x1 ... xm  | 0.1 x1 .. xm 00
     *
     * The forward mapping maps a value to the
     * smallest exponent and mantissa with a
     * reverse mapping not less than the value.
     *
     * The e and m in the forward mapping and reverse are not the same, and the
     * initial increment in from_exp_mant and the decrement in to_exp_mant
     * handle the different ways it is calculating and using the split.
     * This is due to the rounding of bits below the mantissa in the
     * representation, which is confusing but leads to the fastest code.
     *
     * Does not work for value=0.
     ***********************************************/
    template<size_t MANTISSA_BITS, size_t LOW_BITS = 0>
    static size_t to_exp_mant(size_t value)
    {
      size_t LEADING_BIT = one_at_bit(MANTISSA_BITS + LOW_BITS) >> 1;
      size_t MANTISSA_MASK = one_at_bit(MANTISSA_BITS) - 1;

      value = value - 1;

      size_t e =
        bits::BITS - MANTISSA_BITS - LOW_BITS - clz(value | LEADING_BIT);
      size_t b = (e == 0) ? 0 : 1;
      size_t m = (value >> (LOW_BITS + e - b)) & MANTISSA_MASK;

      return (e << MANTISSA_BITS) + m;
    }

    template<size_t MANTISSA_BITS, size_t LOW_BITS = 0>
    constexpr static size_t to_exp_mant_const(size_t value)
    {
      size_t LEADING_BIT = one_at_bit(MANTISSA_BITS + LOW_BITS) >> 1;
      size_t MANTISSA_MASK = one_at_bit(MANTISSA_BITS) - 1;

      value = value - 1;

      size_t e =
        bits::BITS - MANTISSA_BITS - LOW_BITS - clz_const(value | LEADING_BIT);
      size_t b = (e == 0) ? 0 : 1;
      size_t m = (value >> (LOW_BITS + e - b)) & MANTISSA_MASK;

      return (e << MANTISSA_BITS) + m;
    }

    template<size_t MANTISSA_BITS, size_t LOW_BITS = 0>
    constexpr static size_t from_exp_mant(size_t m_e)
    {
      if (MANTISSA_BITS > 0)
      {
        m_e = m_e + 1;
        size_t MANTISSA_MASK = one_at_bit(MANTISSA_BITS) - 1;
        size_t m = m_e & MANTISSA_MASK;
        size_t e = m_e >> MANTISSA_BITS;
        size_t b = e == 0 ? 0 : 1;
        size_t shifted_e = e - b;
        size_t extended_m = (m + (b << MANTISSA_BITS));
        return extended_m << (shifted_e + LOW_BITS);
      }

      return one_at_bit(m_e + LOW_BITS);
    }

    /**
     * Implementation of `std::min`
     *
     * `std::min` is in `<algorithm>`, so pulls in a lot of unneccessary code
     * We write our own to reduce the code that potentially needs reviewing.
     **/
    template<typename T>
    constexpr inline T min(T t1, T t2)
    {
      return t1 < t2 ? t1 : t2;
    }

    /**
     * Implementation of `std::max`
     *
     * `std::max` is in `<algorithm>`, so pulls in a lot of unneccessary code
     * We write our own to reduce the code that potentially needs reviewing.
     **/
    template<typename T>
    constexpr inline T max(T t1, T t2)
    {
      return t1 > t2 ? t1 : t2;
    }
  } // namespace bits
} // namespace snmalloc
