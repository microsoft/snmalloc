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

    using B = capptr::bounds::Chunk;

    static constexpr bool Aligned = true;

    constexpr EmptyRange() = default;

    CapPtr<void, B> alloc_range(size_t)
    {
      return nullptr;
    }
  };
} // namespace snmalloc
