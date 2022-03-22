#pragma once

#include "../ds/address.h"
#include "../pal/pal.h"

namespace snmalloc
{
  template<
    SNMALLOC_CONCEPT(ConceptBackendMetaRange) Pagemap,
    SNMALLOC_CONCEPT(ConceptBackendRange_Alloc) ParentRange>
  class PagemapRegisterRange
  {
    typename ParentRange::State state{};

  public:
    class State
    {
      PagemapRegisterRange range;

    public:
      PagemapRegisterRange* operator->()
      {
        return &range;
      }

      constexpr State() = default;
    };

    constexpr PagemapRegisterRange() = default;

    using B = typename ParentRange::B;
    static_assert(B::spatial >= capptr::dimension::Spatial::Chunk);

    static constexpr bool Aligned = ParentRange::Aligned;

    CapPtr<void, B> alloc_range(size_t size)
    {
      auto base = state->alloc_range(size);

      if (base != nullptr)
        Pagemap::register_range(address_cast(base), size);

      return base;
    }
  };
} // namespace snmalloc
