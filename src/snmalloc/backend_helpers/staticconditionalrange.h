#pragma once
#include "../pal/pal.h"
#include "empty_range.h"
#include "range_helpers.h"

namespace snmalloc
{
  template<typename OptionalRange>
  struct StaticConditionalRange
  {
    template<typename ParentRange>
    class Type : public ContainsParent<Pipe<ParentRange, OptionalRange>>
    {
      using ActualParentRange = Pipe<ParentRange, OptionalRange>;

      using ContainsParent<ActualParentRange>::parent;

      static inline bool disable_range_{false};

    public:
      static constexpr bool Aligned = ActualParentRange::Aligned;

      static constexpr bool ConcurrencySafe = ActualParentRange::ConcurrencySafe;

      using ChunkBounds = typename ActualParentRange::ChunkBounds;
      static_assert(
        ChunkBounds::address_space_control ==
        capptr::dimension::AddressSpaceControl::Full);

      constexpr Type() = default;

      CapPtr<void, ChunkBounds> alloc_range(size_t size)
      {
        if (disable_range_)
        {
          return this->template ancestor<ParentRange>()->alloc_range(size);
        }

        return parent.alloc_range(size);
      }

      void dealloc_range(CapPtr<void, ChunkBounds> base, size_t size)
      {
        if (disable_range_)
        {
          this->template ancestor<ParentRange>()->dealloc_range(base, size);
          return;
        }
        parent.dealloc_range(base, size);
      }

      void disable_range()
      {
        disable_range_ = true;
      }
    };
  };
} // namespace snmalloc
