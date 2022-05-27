#pragma once

#include "../pal/pal.h"
#include "empty_range.h"
#include "range_helpers.h"

namespace snmalloc
{
  template<
    SNMALLOC_CONCEPT(ConceptPagemapMetaRange) Pagemap,
    bool CanConsolidate = true,
    typename ParentRange = EmptyRange>
  class PagemapRegisterRange : public ContainsParent<ParentRange>
  {
    using ContainsParent<ParentRange>::parent;

  public:
    /**
     * We use a nested Apply type to enable a Pipe operation.
     */
    template<typename ParentRange2>
    using Apply = PagemapRegisterRange<Pagemap, CanConsolidate, ParentRange2>;

    constexpr PagemapRegisterRange() = default;

    static constexpr bool Aligned = ParentRange::Aligned;

    static constexpr bool ConcurrencySafe = ParentRange::ConcurrencySafe;

    capptr::Chunk<void> alloc_range(size_t size)
    {
      auto base = parent.alloc_range(size);

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
