#pragma once
#include "bits.h"

#include <cassert>
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
  using address_t = uintptr_t;

  /**
   * Perform pointer arithmetic and return the adjusted pointer.
   */
  template<typename T>
  inline T* pointer_offset(T* base, size_t diff)
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
   * Cast from an address back to a pointer of the specified type.  All uses of
   * this will eventually need auditing for CHERI compatibility.
   */
  template<typename T>
  inline T* pointer_cast(address_t address)
  {
    return reinterpret_cast<T*>(address);
  }

  /**
   * Test if a pointer is aligned to a given size, which must be a power of
   * two.
   */
  template<size_t alignment>
  static inline bool is_aligned_block(void* p, size_t size)
  {
    static_assert(bits::next_pow2_const(alignment) == alignment);

    return ((static_cast<size_t>(address_cast(p)) | size) & (alignment - 1)) ==
      0;
  }

  /**
   * Align a pointer down to a statically specified granularity, which must be a
   * power of two.
   */
  template<size_t alignment, typename T = void>
  inline T* pointer_align_down(void* p)
  {
    static_assert(alignment > 0);
    static_assert(bits::next_pow2_const(alignment) == alignment);
#if __has_builtin(__builtin_align_down)
    return static_cast<T*>(__builtin_align_down(p, alignment));
#else
    return pointer_cast<T>(bits::align_down(address_cast(p), alignment));
#endif
  }

  /**
   * Align a pointer up to a statically specified granularity, which must be a
   * power of two.
   */
  template<size_t alignment, typename T = void>
  inline T* pointer_align_up(void* p)
  {
    static_assert(alignment > 0);
    static_assert(bits::next_pow2_const(alignment) == alignment);
#if __has_builtin(__builtin_align_up)
    return static_cast<T*>(__builtin_align_up(p, alignment));
#else
    return pointer_cast<T>(bits::align_up(address_cast(p), alignment));
#endif
  }
} // namespace snmalloc
