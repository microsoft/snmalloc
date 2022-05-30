#pragma once
#include "../pal/pal.h"
#include "empty_range.h"
#include "range_helpers.h"

namespace snmalloc
{
  template<typename PAL>
  struct CommitRange
  {
    template<typename ParentRange>
    class Type : public ContainsParent<ParentRange>
    {
      using ContainsParent<ParentRange>::parent;

    public:
      static constexpr bool Aligned = ParentRange::Aligned;

      static constexpr bool ConcurrencySafe = ParentRange::ConcurrencySafe;

      constexpr Type() = default;

      capptr::Chunk<void> alloc_range(size_t size)
      {
        SNMALLOC_ASSERT_MSG(
          (size % PAL::page_size) == 0,
          "size ({}) must be a multiple of page size ({})",
          size,
          PAL::page_size);
        auto range = parent.alloc_range(size);
        if (range != nullptr)
          PAL::template notify_using<NoZero>(range.unsafe_ptr(), size);
        return range;
      }

      void dealloc_range(capptr::Chunk<void> base, size_t size)
      {
        SNMALLOC_ASSERT_MSG(
          (size % PAL::page_size) == 0,
          "size ({}) must be a multiple of page size ({})",
          size,
          PAL::page_size);
        PAL::notify_not_using(base.unsafe_ptr(), size);
        parent.dealloc_range(base, size);
      }
    };
  };
} // namespace snmalloc
