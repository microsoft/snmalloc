#pragma once

#ifdef _MSC_VER
#  include <immintrin.h>
#  include <intrin.h>
#  define ALWAYSINLINE __forceinline
#  define NOINLINE __declspec(noinline)
#  define HEADER_GLOBAL __declspec(selectany)
#  define likely(x) !!(x)
#  define unlikely(x) !!(x)
#  define SNMALLOC_SLOW_PATH NOINLINE
#  define SNMALLOC_FAST_PATH ALWAYSINLINE
#  define SNMALLOC_PURE
#else
#  define likely(x) __builtin_expect(!!(x), 1)
#  define unlikely(x) __builtin_expect(!!(x), 0)
#  include <cpuid.h>
#  include <emmintrin.h>
#  define ALWAYSINLINE __attribute__((always_inline))
#  define NOINLINE __attribute__((noinline))
#  define SNMALLOC_SLOW_PATH NOINLINE
#  define SNMALLOC_FAST_PATH inline ALWAYSINLINE
#  define SNMALLOC_PURE __attribute__((const))
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

#ifndef NDEBUG
#  define SNMALLOC_ASSUME(x) assert(x)
#else
#  if __has_builtin(__builtin_assume)
#    define SNMALLOC_ASSUME(x) __builtin_assume((x))
#  else
#    define SNMALLOC_ASSUME(x) \
      do \
      { \
      } while (0)
#  endif
#endif
