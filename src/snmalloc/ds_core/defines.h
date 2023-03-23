#pragma once
#include <cstddef>

#if defined(_MSC_VER) && !defined(__clang__)
// 28 is FAST_FAIL_INVALID_BUFFER_ACCESS.  Not using the symbolic constant to
// avoid depending on winnt.h
#  include <intrin.h> // for __fastfail
#  define SNMALLOC_FAST_FAIL() __fastfail(28)
#  define ALWAYSINLINE __forceinline
#  define NOINLINE __declspec(noinline)
#  define SNMALLOC_LIKELY(x) !!(x)
#  define SNMALLOC_UNLIKELY(x) !!(x)
#  define SNMALLOC_SLOW_PATH NOINLINE
#  define SNMALLOC_FAST_PATH ALWAYSINLINE
/**
 * Fast path with inline linkage.  MSVC assumes that `__forceinline` implies
 * `inline` and complains if you specify `SNMALLOC_FAST_PATH` and `inline`.
 */
#  define SNMALLOC_FAST_PATH_INLINE ALWAYSINLINE
#  if _MSC_VER >= 1927 && !defined(SNMALLOC_USE_CXX17)
#    define SNMALLOC_FAST_PATH_LAMBDA [[msvc::forceinline]]
#  else
#    define SNMALLOC_FAST_PATH_LAMBDA
#  endif
#  define SNMALLOC_PURE
#  define SNMALLOC_COLD
#  define SNMALLOC_REQUIRE_CONSTINIT
#  define SNMALLOC_UNUSED_FUNCTION
#  define SNMALLOC_USED_FUNCTION
#  ifdef SNMALLOC_USE_CXX17
#    define SNMALLOC_NO_UNIQUE_ADDRESS
#  else
#    define SNMALLOC_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#  endif
#else
#  define SNMALLOC_FAST_FAIL() __builtin_trap()
#  define SNMALLOC_LIKELY(x) __builtin_expect(!!(x), 1)
#  define SNMALLOC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define ALWAYSINLINE __attribute__((always_inline))
#  define NOINLINE __attribute__((noinline))
#  define SNMALLOC_SLOW_PATH NOINLINE
#  define SNMALLOC_FAST_PATH ALWAYSINLINE
/**
 * Fast path with inline linkage.  GCC assumes that
 * `__attribute__((always_inline))` is orthogonal to `inline` and complains if
 * you specify `SNMALLOC_FAST_PATH` and don't specify `inline` in places where
 * `inline` would be required for the one definition rule.  The error message
 * in this case is confusing: always-inline function may not be inlined.  If
 * you see this error message when using `SNMALLOC_FAST_PATH` then switch to
 * `SNMALLOC_FAST_PATH_INLINE`.
 */
#  define SNMALLOC_FAST_PATH_INLINE ALWAYSINLINE inline
#  define SNMALLOC_FAST_PATH_LAMBDA SNMALLOC_FAST_PATH
#  define SNMALLOC_PURE __attribute__((const))
#  define SNMALLOC_COLD __attribute__((cold))
#  define SNMALLOC_UNUSED_FUNCTION __attribute((unused))
#  define SNMALLOC_USED_FUNCTION __attribute((used))
#  ifdef SNMALLOC_USE_CXX17
#    define SNMALLOC_NO_UNIQUE_ADDRESS
#  else
#    define SNMALLOC_NO_UNIQUE_ADDRESS [[no_unique_address]]
#  endif
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

namespace snmalloc
{
#ifdef NDEBUG
  static constexpr bool DEBUG = false;
#else
  static constexpr bool DEBUG = true;
#endif

  // Forwards reference so that the platform can define how to handle errors.
  [[noreturn]] SNMALLOC_COLD void error(const char* const str);
} // namespace snmalloc

#define TOSTRING(expr) TOSTRING2(expr)
#define TOSTRING2(expr) #expr

#ifdef __cpp_lib_source_location
#  include <source_location>
#  define SNMALLOC_CURRENT_LINE std::source_location::current().line()
#  define SNMALLOC_CURRENT_FILE std::source_location::current().file_name()
#else
#  define SNMALLOC_CURRENT_LINE TOSTRING(__LINE__)
#  define SNMALLOC_CURRENT_FILE __FILE__
#endif

#ifdef NDEBUG
#  define SNMALLOC_ASSERT_MSG(...) \
    {}
#else
#  define SNMALLOC_ASSERT_MSG(expr, fmt, ...) \
    do \
    { \
      if (!(expr)) \
      { \
        snmalloc::report_fatal_error( \
          "assert fail: {} in {} on {} " fmt "\n", \
          #expr, \
          SNMALLOC_CURRENT_FILE, \
          SNMALLOC_CURRENT_LINE, \
          ##__VA_ARGS__); \
      } \
    } while (0)
#endif
#define SNMALLOC_ASSERT(expr) SNMALLOC_ASSERT_MSG(expr, "")

#define SNMALLOC_CHECK_MSG(expr, fmt, ...) \
  do \
  { \
    if (!(expr)) \
    { \
      snmalloc::report_fatal_error( \
        "Check fail: {} in {} on {} " fmt "\n", \
        #expr, \
        SNMALLOC_CURRENT_FILE, \
        SNMALLOC_CURRENT_LINE, \
        ##__VA_ARGS__); \
    } \
  } while (0)

#define SNMALLOC_CHECK(expr) SNMALLOC_CHECK_MSG(expr, "")

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

namespace snmalloc
{
  /**
   * Forward declaration so that this can be called before the pal header is
   * included.
   */
  template<size_t BufferSize = 1024, typename... Args>
  [[noreturn]] inline void report_fatal_error(Args... args);

  /**
   * Forward declaration so that this can be called before the pal header is
   * included.
   */
  template<size_t BufferSize = 1024, typename... Args>
  inline void message(Args... args);

  template<typename... Args>
  SNMALLOC_FAST_PATH_INLINE void UNUSED(Args&&...)
  {}
} // namespace snmalloc
