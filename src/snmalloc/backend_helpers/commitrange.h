#pragma once

#include "../pal/pal.h"

namespace snmalloc
{
  template<typename ParentRange, typename PAL>
  class CommitRange
  {
    ParentRange parent{};

  public:
    static constexpr bool Aligned = ParentRange::Aligned;

    static constexpr bool ConcurrencySafe = ParentRange::ConcurrencySafe;

    constexpr CommitRange() = default;

    capptr::Chunk<void> alloc_range(size_t size)
    {
      auto range = parent.alloc_range(size);
      if (range != nullptr)
        PAL::template notify_using<NoZero>(range.unsafe_ptr(), size);
      return range;
    }

    void dealloc_range(capptr::Chunk<void> base, size_t size)
    {
      PAL::notify_not_using(base.unsafe_ptr(), size);
      parent.dealloc_range(base, size);
    }
  };
} // namespace snmalloc
