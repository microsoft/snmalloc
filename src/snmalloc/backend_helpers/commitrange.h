#pragma once
#include "../pal/pal.h"
#include "empty_range.h"
#include "range_helpers.h"

namespace snmalloc
{
  template<typename PAL, typename ParentRange = EmptyRange>
  class CommitRange : public ContainsParent<ParentRange>
  {
    using ContainsParent<ParentRange>::parent;

  public:
    /**
     * We use a nested Apply type to enable a Pipe operation.
     */
    template<typename ParentRange2>
    using Apply = CommitRange<PAL, ParentRange2>;

    static constexpr bool Aligned = ParentRange::Aligned;

    static constexpr bool ConcurrencySafe = ParentRange::ConcurrencySafe;

    constexpr CommitRange() = default;

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
} // namespace snmalloc
