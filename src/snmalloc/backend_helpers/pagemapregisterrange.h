#pragma once

#include "../pal/pal.h"
#include "empty_range.h"

namespace snmalloc
{
  template<
    SNMALLOC_CONCEPT(ConceptBackendMetaRange) Pagemap,
    bool CanConsolidate = true,
    typename ParentRange = EmptyRange>
  class PagemapRegisterRange
  {
    ParentRange state{};

  public:
    template<typename ParentRange2>
    using Apply = PagemapRegisterRange<Pagemap, CanConsolidate, ParentRange2>;

    constexpr PagemapRegisterRange() = default;

    static constexpr bool Aligned = ParentRange::Aligned;

    static constexpr bool ConcurrencySafe = ParentRange::ConcurrencySafe;

    capptr::Chunk<void> alloc_range(size_t size)
    {
      auto base = state.alloc_range(size);

      if (base != nullptr)
        Pagemap::register_range(address_cast(base), size);

      if (!CanConsolidate)
      {
        // Mark start of allocation in pagemap.
        auto& entry = Pagemap::get_metaentry_mut(address_cast(base));
        entry.set_boundary();
      }

      return base;
    }
  };
} // namespace snmalloc
