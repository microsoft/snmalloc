#pragma once

#include "snmalloc/ds_core/defines.h"

#ifndef SNMALLOC_USE_SELF_VENDORED_STL
#  define SNMALLOC_USE_SELF_VENDORED_STL 0
#endif

#if SNMALLOC_USE_SELF_VENDORED_STL
#  include "snmalloc/proxy/utility.h"
#  if !defined(__GNUC__) && !defined(__clang__)
#    error "cannot use vendored STL without GNU/Clang extensions"
#  endif

namespace snmalloc
{
  namespace proxy
  {
    template<typename T>
    constexpr SNMALLOC_FAST_PATH const T& max(const T& a, const T& b)
    {
      return a < b ? b : a;
    }

    template<typename T>
    constexpr SNMALLOC_FAST_PATH const T& min(const T& a, const T& b)
    {
      return a < b ? a : b;
    }

    template<typename A, typename B = A>
    constexpr SNMALLOC_FAST_PATH A exchange(A& obj, B&& new_value)
    {
      A old_value = move(obj);
      obj = forward<B>(new_value);
      return old_value;
    }

  } // namespace proxy
} // namespace snmalloc
#else
#  include <algorithm>

namespace snmalloc
{
  namespace proxy
  {
    using std::exchange;
    using std::max;
    using std::min;
  } // namespace proxy
} // namespace snmalloc
#endif
