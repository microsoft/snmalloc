#pragma once

#ifdef _MSC_VER
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
