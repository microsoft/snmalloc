#pragma once
#include "../pal/pal_consts.h"
#include "bits.h"

#include <cstdint>

namespace snmalloc
{
  /**
   * The type used for an address.  Currently, all addresses are assumed to be
   * provenance-carrying values and so it is possible to cast back from the
   * result of arithmetic on an address_t.  Eventually, this will want to be
   * separated into two types, one for raw addresses and one for addresses that
   * can be cast back to pointers.
   */
  using address_t = Aal::address_t;

  /**
   * Perform pointer arithmetic and return the adjusted pointer.
   */
  template<typename T>
  inline T* pointer_offset(T* base, size_t diff)
  {
    return reinterpret_cast<T*>(reinterpret_cast<char*>(base) + diff);
  }

  /**
   * Perform pointer arithmetic and return the adjusted pointer.
   */
  template<typename T>
  inline T* pointer_offset_signed(T* base, ptrdiff_t diff)
  {
    return reinterpret_cast<T*>(reinterpret_cast<char*>(base) + diff);
  }

  /**
   * Cast from a pointer type to an address.
   */
  template<typename T>
  inline address_t address_cast(T* ptr)
  {
    return reinterpret_cast<address_t>(ptr);
  }

  /**
   * Test if a pointer is aligned to a given size, which must be a power of
   * two.
   */
  template<size_t alignment>
  static inline bool is_aligned_block(void* p, size_t size)
  {
    static_assert(bits::is_pow2(alignment));

    return ((address_cast(p) | size) & (alignment - 1)) == 0;
  }

  /**
   * Align a pointer down to a statically specified granularity, which must be a
   * power of two.
   */
  template<size_t alignment, typename T = void>
  SNMALLOC_FAST_PATH T* pointer_align_down(void* p)
  {
    static_assert(alignment > 0);
    static_assert(bits::is_pow2(alignment));
    if constexpr (alignment == 1)
      return static_cast<T*>(p);
    else
    {
#if __has_builtin(__builtin_align_down)
      return static_cast<T*>(__builtin_align_down(p, alignment));
#else
      return reinterpret_cast<T*>(
        bits::align_down(reinterpret_cast<uintptr_t>(p), alignment));
#endif
    }
  }

  /**
   * Align a pointer up to a statically specified granularity, which must be a
   * power of two.
   */
  template<size_t alignment, typename T = void>
  inline T* pointer_align_up(void* p)
  {
    static_assert(alignment > 0);
    static_assert(bits::is_pow2(alignment));
    if constexpr (alignment == 1)
      return static_cast<T*>(p);
    else
    {
#if __has_builtin(__builtin_align_up)
      return static_cast<T*>(__builtin_align_up(p, alignment));
#else
      return reinterpret_cast<T*>(
        bits::align_up(reinterpret_cast<uintptr_t>(p), alignment));
#endif
    }
  }

  /**
   * Align a pointer down to a dynamically specified granularity, which must be
   * a power of two.
   */
  template<typename T = void>
  SNMALLOC_FAST_PATH T* pointer_align_down(void* p, size_t alignment)
  {
    SNMALLOC_ASSERT(alignment > 0);
    SNMALLOC_ASSERT(bits::is_pow2(alignment));
#if __has_builtin(__builtin_align_down)
    return static_cast<T*>(__builtin_align_down(p, alignment));
#else
    return reinterpret_cast<T*>(
      bits::align_down(reinterpret_cast<uintptr_t>(p), alignment));
#endif
  }

  /**
   * Align a pointer up to a dynamically specified granularity, which must
   * be a power of two.
   */
  template<typename T = void>
  inline T* pointer_align_up(void* p, size_t alignment)
  {
    SNMALLOC_ASSERT(alignment > 0);
    SNMALLOC_ASSERT(bits::is_pow2(alignment));
#if __has_builtin(__builtin_align_up)
    return static_cast<T*>(__builtin_align_up(p, alignment));
#else
    return reinterpret_cast<T*>(
      bits::align_up(reinterpret_cast<uintptr_t>(p), alignment));
#endif
  }

  /**
   * Compute the difference in pointers in units of char.  base is
   * expected to point to the base of some (sub)allocation into which cursor
   * points; would-be negative answers trip an assertion in debug builds.
   */
  inline size_t pointer_diff(void* base, void* cursor)
  {
    SNMALLOC_ASSERT(cursor >= base);
    return static_cast<size_t>(
      static_cast<char*>(cursor) - static_cast<char*>(base));
  }

  /**
   * Compute the difference in pointers in units of char. This can be used
   * across allocations.
   */
  inline ptrdiff_t pointer_diff_signed(void* base, void* cursor)
  {
    return static_cast<ptrdiff_t>(
      static_cast<char*>(cursor) - static_cast<char*>(base));
  }

} // namespace snmalloc
