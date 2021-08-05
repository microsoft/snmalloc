#pragma once
#include "../pal/pal_consts.h"
#include "bits.h"
#include "ptrwrap.h"

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
   * Perform arithmetic on a uintptr_t.
   */
  inline uintptr_t pointer_offset(uintptr_t base, size_t diff)
  {
    return base + diff;
  }

  /**
   * Perform pointer arithmetic and return the adjusted pointer.
   */
  template<typename U = void, typename T>
  inline U* pointer_offset(T* base, size_t diff)
  {
    SNMALLOC_ASSERT(base != nullptr); /* Avoid UB */
    return reinterpret_cast<U*>(
      reinterpret_cast<uintptr_t>(base) + static_cast<uintptr_t>(diff));
  }

  template<enum capptr_bounds bounds, typename T>
  inline CapPtr<void, bounds>
  pointer_offset(CapPtr<T, bounds> base, size_t diff)
  {
    return CapPtr<void, bounds>(pointer_offset(base.unsafe_ptr(), diff));
  }

  /**
   * Perform pointer arithmetic and return the adjusted pointer.
   */
  template<typename U = void, typename T>
  inline U* pointer_offset_signed(T* base, ptrdiff_t diff)
  {
    SNMALLOC_ASSERT(base != nullptr); /* Avoid UB */
    return reinterpret_cast<U*>(reinterpret_cast<char*>(base) + diff);
  }

  template<enum capptr_bounds bounds, typename T>
  inline CapPtr<void, bounds>
  pointer_offset_signed(CapPtr<T, bounds> base, ptrdiff_t diff)
  {
    return CapPtr<void, bounds>(pointer_offset_signed(base.unsafe_ptr(), diff));
  }

  /**
   * Cast from a pointer type to an address.
   */
  template<typename T>
  inline address_t address_cast(T* ptr)
  {
    return reinterpret_cast<address_t>(ptr);
  }

  /*
   * Provide address_cast methods for the provenance-hinting pointer wrapper
   * types as well.  While we'd prefer that these be methods on the wrapper
   * type, they have to be defined later, because the AAL both define address_t,
   * as per above, and uses the wrapper types in its own definition, e.g., of
   * capptr_bound.
   */

  template<typename T, enum capptr_bounds bounds>
  inline address_t address_cast(CapPtr<T, bounds> a)
  {
    return address_cast(a.unsafe_ptr());
  }

  /**
   * Test if a pointer is aligned to a given size, which must be a power of
   * two.
   */
  template<size_t alignment>
  static inline bool is_aligned_block(address_t p, size_t size)
  {
    static_assert(bits::is_pow2(alignment));

    return ((p | size) & (alignment - 1)) == 0;
  }

  template<size_t alignment>
  static inline bool is_aligned_block(void* p, size_t size)
  {
    return is_aligned_block<alignment>(address_cast(p), size);
  }

  /**
   * Align a uintptr_t down to a statically specified granularity, which must be
   * a power of two.
   */
  template<size_t alignment>
  inline uintptr_t pointer_align_down(uintptr_t p)
  {
    static_assert(alignment > 0);
    static_assert(bits::is_pow2(alignment));
    if constexpr (alignment == 1)
      return p;
    else
    {
#if __has_builtin(__builtin_align_down)
      return __builtin_align_down(p, alignment);
#else
      return bits::align_down(p, alignment);
#endif
    }
  }

  /**
   * Align a pointer down to a statically specified granularity, which must be a
   * power of two.
   */
  template<size_t alignment, typename T = void>
  inline T* pointer_align_down(void* p)
  {
    return reinterpret_cast<T*>(
      pointer_align_down<alignment>(reinterpret_cast<uintptr_t>(p)));
  }

  template<size_t alignment, typename T, capptr_bounds bounds>
  inline CapPtr<T, bounds> pointer_align_down(CapPtr<void, bounds> p)
  {
    return CapPtr<T, bounds>(pointer_align_down<alignment, T>(p.unsafe_ptr()));
  }

  template<size_t alignment>
  inline address_t address_align_down(address_t p)
  {
    return bits::align_down(p, alignment);
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

  template<size_t alignment, typename T = void, enum capptr_bounds bounds>
  inline CapPtr<T, bounds> pointer_align_up(CapPtr<void, bounds> p)
  {
    return CapPtr<T, bounds>(pointer_align_up<alignment, T>(p.unsafe_ptr()));
  }

  template<size_t alignment>
  inline address_t address_align_up(address_t p)
  {
    return bits::align_up(p, alignment);
  }

  /**
   * Align a pointer down to a dynamically specified granularity, which must be
   * a power of two.
   */
  template<typename T = void>
  inline T* pointer_align_down(void* p, size_t alignment)
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

  template<typename T = void, enum capptr_bounds bounds>
  inline CapPtr<T, bounds>
  pointer_align_down(CapPtr<void, bounds> p, size_t alignment)
  {
    return CapPtr<T, bounds>(pointer_align_down<T>(p.unsafe_ptr(), alignment));
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

  template<typename T = void, enum capptr_bounds bounds>
  inline CapPtr<T, bounds>
  pointer_align_up(CapPtr<void, bounds> p, size_t alignment)
  {
    return CapPtr<T, bounds>(pointer_align_up<T>(p.unsafe_ptr(), alignment));
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

  template<
    typename T = void,
    typename U = void,
    enum capptr_bounds Tbounds,
    enum capptr_bounds Ubounds>
  inline size_t pointer_diff(CapPtr<T, Tbounds> base, CapPtr<U, Ubounds> cursor)
  {
    return pointer_diff(base.unsafe_ptr(), cursor.unsafe_ptr());
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

  template<
    typename T = void,
    typename U = void,
    enum capptr_bounds Tbounds,
    enum capptr_bounds Ubounds>
  inline ptrdiff_t
  pointer_diff_signed(CapPtr<T, Tbounds> base, CapPtr<U, Ubounds> cursor)
  {
    return pointer_diff_signed(base.unsafe_ptr(), cursor.unsafe_ptr());
  }

} // namespace snmalloc
