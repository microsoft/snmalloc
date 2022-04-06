#pragma once

#include "../ds/ds.h"

namespace snmalloc
{
  /**
   * Makes the supplied ParentRange into a global variable,
   * and protects access with a lock.
   */
  template<typename ParentRange>
  class GlobalRange
  {
    typename ParentRange::State parent{};

    /**
     * This is infrequently used code, a spin lock simplifies the code
     * considerably, and should never be on the fast path.
     */
    FlagWord spin_lock{};

  public:
    class State
    {
      SNMALLOC_REQUIRE_CONSTINIT static inline GlobalRange global_range{};

    public:
      constexpr GlobalRange* operator->()
      {
        return &global_range;
      }

      constexpr State() = default;
    };

    static constexpr bool Aligned = ParentRange::Aligned;

    static constexpr bool ConcurrencySafe = true;

    constexpr GlobalRange() = default;

    capptr::Chunk<void> alloc_range(size_t size)
    {
      FlagLock lock(spin_lock);
      return parent->alloc_range(size);
    }

    void dealloc_range(capptr::Chunk<void> base, size_t size)
    {
      FlagLock lock(spin_lock);
      parent->dealloc_range(base, size);
    }
  };
} // namespace snmalloc
