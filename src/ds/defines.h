#pragma once

#if defined(_MSC_VER) && !defined(__clang__)
#  define ALWAYSINLINE __forceinline
#  define NOINLINE __declspec(noinline)
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
#endif

#if !defined(__clang__) && defined(__GNUC__)
#  if __GNUC__ >= 8
#    define GCC_VERSION_EIGHT_PLUS
#  endif
#endif

#ifndef __has_builtin
#  define __has_builtin(x) 0
#endif

#define UNUSED(x) ((void)(x))

namespace snmalloc
{
  // Forwards reference so that the platform can define how to handle errors.
  void error(const char* const str);
} // namespace snmalloc

#ifdef NDEBUG
#  define SNMALLOC_ASSERT(expr) \
    {}
#else
#  define SNMALLOC_ASSERT(expr) \
    { \
      if (!(expr)) \
      { \
        snmalloc::error("assert fail"); \
      } \
    }
#endif

#ifndef NDEBUG
#  define SNMALLOC_ASSUME(x) SNMALLOC_ASSERT(x)
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
