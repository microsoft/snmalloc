#pragma once

#include "../pal/pal.h"
#include "empty_range.h"
#include "range_helpers.h"

namespace snmalloc
{
  template<
    SNMALLOC_CONCEPT(IsWritablePagemapWithRegister) Pagemap,
    bool CanConsolidate = true>
  struct PagemapRegisterRange
  {
    template<typename ParentRange = EmptyRange>
    class Type : public ContainsParent<ParentRange>
    {
      using ContainsParent<ParentRange>::parent;

    public:
      constexpr Type() = default;

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
  };
} // namespace snmalloc
