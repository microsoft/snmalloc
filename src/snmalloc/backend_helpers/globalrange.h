#pragma once

#include "../ds/ds.h"
#include "empty_range.h"

namespace snmalloc
{
  /**
   * Makes the supplied ParentRange into a global variable,
   * and protects access with a lock.
   */
  struct GlobalRange
  {
    template<typename ParentRange = EmptyRange<>>
    class Type : public StaticParent<ParentRange>
    {
      using StaticParent<ParentRange>::parent;

      /**
       * This is infrequently used code, a spin lock simplifies the code
       * considerably, and should never be on the fast path.
       */
      SNMALLOC_REQUIRE_CONSTINIT static inline FlagWord spin_lock{};

    public:
      static constexpr bool Aligned = ParentRange::Aligned;

      static constexpr bool ConcurrencySafe = true;

      using ChunkBounds = typename ParentRange::ChunkBounds;

      constexpr Type() = default;

      CapPtr<void, ChunkBounds> alloc_range(size_t size)
      {
        FlagLock lock(spin_lock);
        return parent.alloc_range(size);
      }

      void dealloc_range(CapPtr<void, ChunkBounds> base, size_t size)
      {
        FlagLock lock(spin_lock);
        parent.dealloc_range(base, size);
      }
    };
  };
} // namespace snmalloc
