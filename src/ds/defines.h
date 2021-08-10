#pragma once

#if defined(_MSC_VER) && !defined(__clang__)
#  define ALWAYSINLINE __forceinline
#  define NOINLINE __declspec(noinline)
#  define likely(x) !!(x)
#  define unlikely(x) !!(x)
#  define SNMALLOC_SLOW_PATH NOINLINE
#  define SNMALLOC_FAST_PATH ALWAYSINLINE
#  if _MSC_VER >= 1927 && !defined(SNMALLOC_USE_CXX17)
#    define SNMALLOC_FAST_PATH_LAMBDA [[msvc::forceinline]]
#  else
#    define SNMALLOC_FAST_PATH_LAMBDA
#  endif
#  define SNMALLOC_PURE
#  define SNMALLOC_COLD
#  define SNMALLOC_REQUIRE_CONSTINIT
#  define SNMALLOC_UNUSED_FUNCTION
#else
#  define likely(x) __builtin_expect(!!(x), 1)
#  define unlikely(x) __builtin_expect(!!(x), 0)
#  define ALWAYSINLINE __attribute__((always_inline))
#  define NOINLINE __attribute__((noinline))
#  define SNMALLOC_SLOW_PATH NOINLINE
#  define SNMALLOC_FAST_PATH ALWAYSINLINE
#  define SNMALLOC_FAST_PATH_LAMBDA SNMALLOC_FAST_PATH
#  define SNMALLOC_PURE __attribute__((const))
#  define SNMALLOC_COLD __attribute__((cold))
#  define SNMALLOC_UNUSED_FUNCTION __attribute((unused))
#  ifdef __clang__
#    define SNMALLOC_REQUIRE_CONSTINIT \
      [[clang::require_constant_initialization]]
#  else
#    define SNMALLOC_REQUIRE_CONSTINIT
#  endif
#endif

#if defined(__cpp_constinit) && __cpp_constinit >= 201907
#  define SNMALLOC_CONSTINIT_FN constinit
#  define SNMALLOC_CONSTINIT_STATIC constinit const
#else
#  define SNMALLOC_CONSTINIT_FN constexpr
#  define SNMALLOC_CONSTINIT_STATIC constexpr
#endif

#if defined(__cpp_consteval)
#  define SNMALLOC_CONSTEVAL consteval
#else
#  define SNMALLOC_CONSTEVAL constexpr
#endif

#if !defined(__clang__) && defined(__GNUC__)
#  define GCC_NOT_CLANG
#endif

#ifdef GCC_NOT_CLANG
#  if __GNUC__ >= 8
#    define GCC_VERSION_EIGHT_PLUS
#  endif
#endif

#ifdef __APPLE__
#  define SNMALLOC_FORCE_BSS __attribute__((section("__DATA,__bss")))
#elif defined(__ELF__)
#  define SNMALLOC_FORCE_BSS __attribute__((section(".bss")))
#else
#  define SNMALLOC_FORCE_BSS
#endif

#ifndef __has_builtin
#  define __has_builtin(x) 0
#endif

#define UNUSED(x) ((void)(x))

namespace snmalloc
{
  // Forwards reference so that the platform can define how to handle errors.
  [[noreturn]] SNMALLOC_COLD void error(const char* const str);
} // namespace snmalloc

#define TOSTRING(expr) TOSTRING2(expr)
#define TOSTRING2(expr) #expr

#ifdef NDEBUG
#  define SNMALLOC_ASSERT(expr) \
    {}
#else
#  define SNMALLOC_ASSERT(expr) \
    { \
      if (!(expr)) \
      { \
        snmalloc::error("assert fail: " #expr " in " __FILE__ \
                        " on " TOSTRING(__LINE__)); \
      } \
    }
#endif

#define SNMALLOC_CHECK(expr) \
  { \
    if (!(expr)) \
    { \
      snmalloc::error("Check fail: " #expr " in " __FILE__ \
                      " on " TOSTRING(__LINE__)); \
    } \
  }

#ifndef NDEBUG
#  define SNMALLOC_ASSUME(x) SNMALLOC_ASSERT(x)
#else
#  if __has_builtin(__builtin_assume)
#    define SNMALLOC_ASSUME(x) __builtin_assume((x))
#  elif defined(_MSC_VER)
#    define SNMALLOC_ASSUME(x) __assume((x));
#  elif defined(__GNUC__)
#    define SNMALLOC_ASSUME(x) \
      if (!(x)) \
        __builtin_unreachable();
#  else
#    define SNMALLOC_ASSUME(x) \
      do \
      { \
      } while (0)
#  endif
#endif

inline SNMALLOC_FAST_PATH void check_client_error(const char* const str)
{
  //[[clang::musttail]]
  return snmalloc::error(str);
}

inline SNMALLOC_FAST_PATH void
check_client_impl(bool test, const char* const str)
{
  if (unlikely(!test))
    check_client_error(str);
}
#ifdef SNMALLOC_CHECK_CLIENT
#  define check_client(test, str) check_client_impl(test, str)
#else
#  define check_client(test, str)
#endif

namespace snmalloc
{
#ifdef SNMALLOC_CHECK_CLIENT
  static constexpr bool CHECK_CLIENT = true;
#else
  static constexpr bool CHECK_CLIENT = false;
#endif
} // namespace snmalloc
