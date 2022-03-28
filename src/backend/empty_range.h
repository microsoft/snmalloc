#pragma once
#include "../ds/ptrwrap.h"

namespace snmalloc
{
  class EmptyRange
  {
  public:
    class State
    {
    public:
      EmptyRange* operator->()
      {
        static EmptyRange range{};
        return &range;
      }

      constexpr State() = default;
    };

    static constexpr bool Aligned = true;

    static constexpr bool ConcurrencySafe = true;

    constexpr EmptyRange() = default;

    capptr::Chunk<void> alloc_range(size_t)
    {
      return nullptr;
    }
  };
} // namespace snmalloc
