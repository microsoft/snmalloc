#pragma once
#include "../ds_core/ds_core.h"

namespace snmalloc
{
  class EmptyRange
  {
  public:
    static constexpr bool Aligned = true;

    static constexpr bool ConcurrencySafe = true;

    constexpr EmptyRange() = default;

    capptr::Chunk<void> alloc_range(size_t)
    {
      return nullptr;
    }
  };
} // namespace snmalloc
