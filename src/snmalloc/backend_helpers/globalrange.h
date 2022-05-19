#pragma once

#include "../ds/ds.h"
#include "empty_range.h"

namespace snmalloc
{
  /**
   * Makes the supplied ParentRange into a global variable,
   * and protects access with a lock.
   */
  template<typename ParentRange = EmptyRange>
  class GlobalRange : public StaticParent<ParentRange>
  {
    using StaticParent<ParentRange>::parent;

    /**
     * This is infrequently used code, a spin lock simplifies the code
     * considerably, and should never be on the fast path.
     */
    SNMALLOC_REQUIRE_CONSTINIT static inline FlagWord spin_lock{};

  public:
    /**
     * We use a nested Apply type to enable a Pipe operation.
     */
    template<typename ParentRange2>
    using Apply = GlobalRange<ParentRange2>;

    static constexpr bool Aligned = ParentRange::Aligned;

    static constexpr bool ConcurrencySafe = true;

    constexpr GlobalRange() = default;

    capptr::Chunk<void> alloc_range(size_t size)
    {
      FlagLock lock(spin_lock);
      return parent.alloc_range(size);
    }

    void dealloc_range(capptr::Chunk<void> base, size_t size)
    {
      FlagLock lock(spin_lock);
      parent.dealloc_range(base, size);
    }
  };
} // namespace snmalloc
