#pragma once

#ifndef SNMALLOC_USE_SELF_VENDORED_STL
#  define SNMALLOC_USE_SELF_VENDORED_STL 0
#endif

#if SNMALLOC_USE_SELF_VENDORED_STL

#  include "snmalloc/proxy/type_traits.h"

#  if !defined(__GNUC__) && !defined(__clang__)
#    error "cannot use vendored STL without GNU/Clang extensions"
#  endif

#  if __has_cpp_attribute(_Clang::__lifetimebound__)
#    define SNMALLOC_LIFETIMEBOUND [[_Clang::__lifetimebound__]]
#  else
#    define SNMALLOC_LIFETIMEBOUND
#  endif

namespace snmalloc
{
  namespace proxy
  {
    template<class T>
    [[nodiscard]] inline constexpr T&&
    forward(remove_reference_t<T>& ref) noexcept
    {
      return static_cast<T&&>(ref);
    }

    template<class T>
    [[nodiscard]] inline constexpr T&&
    forward(SNMALLOC_LIFETIMEBOUND remove_reference_t<T>&& ref) noexcept
    {
      static_assert(
        !is_lvalue_reference_v<T>, "cannot forward an rvalue as an lvalue");
      return static_cast<T&&>(ref);
    }

    template<class T>
    [[nodiscard]] inline constexpr remove_reference_t<T>&&
    move(SNMALLOC_LIFETIMEBOUND T&& ref) noexcept
    {
#  ifdef __clang__
      using U [[gnu::nodebug]] = remove_reference_t<T>;
#  else
      using U = remove_reference_t<T>;
#  endif
      return static_cast<U&&>(ref);
    }

#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated"
    template<class T>
    T&& declval_impl(int);
    template<class T>
    T declval_impl(long);
#  pragma GCC diagnostic pop

    template<class T>
    constexpr inline decltype(declval_impl<T>(0)) declval() noexcept
    {
      static_assert(
        !is_same_v<T, T>, "declval cannot be used in an evaluation context");
    }

    template<class T1, class T2>
    struct Pair
    {
      T1 first;
      T2 second;
    };
  } // namespace proxy
} // namespace snmalloc
#else

#  include <utility>

namespace snmalloc
{
  namespace proxy
  {
    using std::declval;
    using std::forward;
    using std::move;
    template<class T1, class T2>
    using Pair = std::pair<T1, T2>;
  } // namespace proxy
} // namespace snmalloc
#endif
