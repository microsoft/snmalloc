#pragma once

#include "defines.h"

#include <stddef.h>

namespace snmalloc
{
  /**
   * A simple fixed-size array container.
   *
   * This provides a std::array-like interface without depending on the
   * standard library. The array supports aggregate initialization and
   * provides iterators for range-based for loops.
   *
   * @tparam T The element type
   * @tparam N The number of elements
   */
  template<typename T, size_t N>
  struct Array
  {
    // Expose this to public to allow aggregate initialization.
    T storage_[N];

    [[nodiscard]] constexpr SNMALLOC_FAST_PATH size_t size() const
    {
      return N;
    }

    constexpr T& operator[](size_t i)
    {
      return storage_[i];
    }

    constexpr const T& operator[](size_t i) const
    {
      return storage_[i];
    }

    using value_type = T;
    using size_type = size_t;
    using iterator = T*;
    using const_iterator = const T*;

    [[nodiscard]] constexpr SNMALLOC_FAST_PATH iterator begin()
    {
      return &storage_[0];
    }

    [[nodiscard]] constexpr SNMALLOC_FAST_PATH const_iterator begin() const
    {
      return &storage_[0];
    }

    [[nodiscard]] constexpr SNMALLOC_FAST_PATH iterator end()
    {
      return &storage_[N];
    }

    [[nodiscard]] constexpr SNMALLOC_FAST_PATH const_iterator end() const
    {
      return &storage_[N];
    }

    [[nodiscard]] constexpr SNMALLOC_FAST_PATH T* data()
    {
      return &storage_[0];
    }

    [[nodiscard]] constexpr SNMALLOC_FAST_PATH const T* data() const
    {
      return &storage_[0];
    }
  };

  /**
   * Specialization for zero-length arrays.
   *
   * Zero-length arrays are not valid in standard C++, so we provide
   * a specialization that has no storage but maintains the same interface.
   */
  template<typename T>
  struct Array<T, 0>
  {
    [[nodiscard]] constexpr SNMALLOC_FAST_PATH size_t size() const
    {
      return 0;
    }

    constexpr T& operator[](size_t)
    {
      SNMALLOC_FAST_FAIL();
    }

    constexpr const T& operator[](size_t) const
    {
      SNMALLOC_FAST_FAIL();
    }

    using value_type = T;
    using size_type = size_t;
    using iterator = T*;
    using const_iterator = const T*;

    [[nodiscard]] constexpr SNMALLOC_FAST_PATH iterator begin()
    {
      return nullptr;
    }

    [[nodiscard]] constexpr SNMALLOC_FAST_PATH const_iterator begin() const
    {
      return nullptr;
    }

    [[nodiscard]] constexpr SNMALLOC_FAST_PATH iterator end()
    {
      return nullptr;
    }

    [[nodiscard]] constexpr SNMALLOC_FAST_PATH const_iterator end() const
    {
      return nullptr;
    }

    [[nodiscard]] constexpr SNMALLOC_FAST_PATH T* data()
    {
      return nullptr;
    }

    [[nodiscard]] constexpr SNMALLOC_FAST_PATH const T* data() const
    {
      return nullptr;
    }
  };

  // Free function begin/end for Array
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

  // Free function begin/end for C-style arrays
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
} // namespace snmalloc
