#pragma once
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
} // namespace snmalloc
