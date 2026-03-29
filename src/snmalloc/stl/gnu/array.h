#pragma once

#include "snmalloc/ds_core/defines.h"

#include <stddef.h>

namespace snmalloc
{
  namespace stl
  {
    template<typename T, size_t N>
    struct Array
    {
      // N is potentially 0, so we mark it with __extension__ attribute.
      // Expose this to public to allow aggregate initialization
      __extension__ T _storage[N];

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH size_t size() const
      {
        return N;
      }

      constexpr T& operator[](size_t i) SNMALLOC_LIFETIMEBOUND
      {
        return _storage[i];
      }

      constexpr const T& operator[](size_t i) const SNMALLOC_LIFETIMEBOUND
      {
        return _storage[i];
      }

      using value_type = T;
      using size_type = size_t;
      using iterator = T*;
      using const_iterator = const T*;

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH iterator begin()
        SNMALLOC_LIFETIMEBOUND
      {
        return &_storage[0];
      }

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH const_iterator begin() const
        SNMALLOC_LIFETIMEBOUND
      {
        return &_storage[0];
      }

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH iterator end()
        SNMALLOC_LIFETIMEBOUND
      {
        return &_storage[N];
      }

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH const_iterator end() const
        SNMALLOC_LIFETIMEBOUND
      {
        return &_storage[N];
      }

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH T* data()
        SNMALLOC_LIFETIMEBOUND
      {
        return &_storage[0];
      }

      [[nodiscard]] constexpr SNMALLOC_FAST_PATH const T* data() const
        SNMALLOC_LIFETIMEBOUND
      {
        return &_storage[0];
      }
    };

    template<typename T, size_t N>
    constexpr T* begin(SNMALLOC_LIFETIMEBOUND Array<T, N>& a)
    {
      return a.begin();
    }

    template<typename T, size_t N>
    constexpr T* end(SNMALLOC_LIFETIMEBOUND Array<T, N>& a)
    {
      return a.end();
    }

    template<typename T, size_t N>
    constexpr const T* begin(SNMALLOC_LIFETIMEBOUND const Array<T, N>& a)
    {
      return a.begin();
    }

    template<typename T, size_t N>
    constexpr const T* end(SNMALLOC_LIFETIMEBOUND const Array<T, N>& a)
    {
      return a.end();
    }

    template<typename T, size_t N>
    constexpr T* begin(SNMALLOC_LIFETIMEBOUND T (&a)[N])
    {
      return &a[0];
    }

    template<typename T, size_t N>
    constexpr T* end(SNMALLOC_LIFETIMEBOUND T (&a)[N])
    {
      return &a[N];
    }

    template<typename T, size_t N>
    constexpr const T* begin(SNMALLOC_LIFETIMEBOUND const T (&a)[N])
    {
      return &a[0];
    }

    template<typename T, size_t N>
    constexpr const T* end(SNMALLOC_LIFETIMEBOUND const T (&a)[N])
    {
      return &a[N];
    }
  } // namespace stl
} // namespace snmalloc
