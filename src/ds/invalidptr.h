#pragma once

#include "address.h"

namespace snmalloc
{
  /**
   * Invalid pointer class.  This is similar to `std::nullptr_t`, but allows
   * other values.
   */
  template<address_t Sentinel>
  struct InvalidPointer
  {
    /**
     * Equality comparison. Two invalid pointer values with the same sentinel
     * are always the same, invalid pointer values with different sentinels are
     * always different.
     */
    template<address_t OtherSentinel>
    constexpr bool operator==(const InvalidPointer<OtherSentinel>&)
    {
      return Sentinel == OtherSentinel;
    }
    /**
     * Equality comparison. Two invalid pointer values with the same sentinel
     * are always the same, invalid pointer values with different sentinels are
     * always different.
     */
    template<address_t OtherSentinel>
    constexpr bool operator!=(const InvalidPointer<OtherSentinel>&)
    {
      return Sentinel != OtherSentinel;
    }
    /**
     * Implicit conversion, creates a pointer with the value of the sentinel.
     * On CHERI and other provenance-tracking systems, this is a
     * provenance-free integer and so will trap if dereferenced, on other
     * systems the sentinel should be a value in unmapped memory.
     */
    template<typename T>
    operator T*() const
    {
      return reinterpret_cast<T*>(Sentinel);
    }
    /**
     * Implicit conversion to an address, returns the sentinel value.
     */
    operator address_t() const
    {
      return Sentinel;
    }
  };
} // namespace snmalloc
