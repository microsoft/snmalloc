#pragma once

#include "snmalloc/ds_core/defines.h"

#ifndef SNMALLOC_USE_SELF_VENDORED_STL
#  define SNMALLOC_USE_SELF_VENDORED_STL 0
#endif

#if SNMALLOC_USE_SELF_VENDORED_STL

#  if !defined(__GNUC__) && !defined(__clang__)
#    error "cannot use vendored STL without GNU/Clang extensions"
#  endif

#  include <stddef.h>

namespace snmalloc
{
  namespace proxy
  {
    template<typename T, size_t N>
    struct Array
    {
      // N is potentially 0, so we mark it with __extension__ attribute.
      // expose this to public to allow aggregate initialization
      __extension__ T storage[N];

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH size_t size() const
      {
        return N;
      }

      constexpr T& operator[](size_t i)
      {
        return storage[i];
      }

      constexpr const T& operator[](size_t i) const
      {
        return storage[i];
      }

      using value_type = T;
      using size_type = size_t;
      using iterator = T*;
      using const_iterator = const T*;

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH iterator begin()
      {
        return &storage[0];
      }

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH const_iterator begin() const
      {
        return &storage[0];
      }

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH iterator end()
      {
        return &storage[N];
      }

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH const_iterator end() const
      {
        return &storage[N];
      }

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH T* data()
      {
        return &storage[0];
      }

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH const T* data() const
      {
        return &storage[0];
      }
    };

    template<typename T, size_t N>
    constexpr T* begin(Array<T, N>& a)
    {
      return a.begin();
    }

    template<typename T, size_t N>
    constexpr T* end(Array<T, N>& a)
    {
      return a.end();
    }

    template<typename T, size_t N>
    constexpr const T* begin(const Array<T, N>& a)
    {
      return a.begin();
    }

    template<typename T, size_t N>
    constexpr const T* end(const Array<T, N>& a)
    {
      return a.end();
    }

    template<typename T, size_t N>
    constexpr T* begin(T (&a)[N])
    {
      return &a[0];
    }

    template<typename T, size_t N>
    constexpr T* end(T (&a)[N])
    {
      return &a[N];
    }

    template<typename T, size_t N>
    constexpr const T* begin(const T (&a)[N])
    {
      return &a[0];
    }

    template<typename T, size_t N>
    constexpr const T* end(const T (&a)[N])
    {
      return &a[N];
    }
  } // namespace proxy
} // namespace snmalloc
#else
#  include <array>

namespace snmalloc
{
  namespace proxy
  {
    template<typename T, size_t N>
    using Array = std::array<T, N>;

    using std::begin;
    using std::end;
  } // namespace proxy
} // namespace snmalloc
#endif
