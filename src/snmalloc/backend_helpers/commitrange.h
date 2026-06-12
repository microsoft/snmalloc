#pragma once
#include "../pal/pal.h"
#include "empty_range.h"
#include "fragstats.h"
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

      using ChunkBounds = typename ParentRange::ChunkBounds;
      static_assert(
        ChunkBounds::address_space_control ==
        capptr::dimension::AddressSpaceControl::Full);

      constexpr Type() = default;

      CapPtr<void, ChunkBounds> alloc_range(size_t size)
      {
        SNMALLOC_ASSERT_MSG(
          (size % PAL::page_size) == 0,
          "size ({}) must be a multiple of page size ({})",
          size,
          PAL::page_size);
        auto range = parent.alloc_range(size);
        if (range != nullptr)
        {
          auto result =
            PAL::template notify_using<NoZero>(range.unsafe_ptr(), size);
          if (!result)
          {
            // If notify_using fails, we deallocate the range and return
            // nullptr.
            parent.dealloc_range(range, size);
            return CapPtr<void, ChunkBounds>(nullptr);
          }

          // Phase 9.4 -- record successful commit for FullAllocStats.
          // Skipped on the failure path above so the counter only
          // reflects pages the PAL actually accepted.
          BackendFragCounters::on_commit(size);
        }
        return range;
      }

      void dealloc_range(CapPtr<void, ChunkBounds> base, size_t size)
      {
        SNMALLOC_ASSERT_MSG(
          (size % PAL::page_size) == 0,
          "size ({}) must be a multiple of page size ({})",
          size,
          PAL::page_size);
        PAL::notify_not_using(base.unsafe_ptr(), size);
        // Phase 9.4 -- record the decommit for FullAllocStats.  The
        // PAL hook itself returns void, so we mirror the alloc-side
        // semantics: every dealloc that reaches here is treated as a
        // successful release back to the OS.
        BackendFragCounters::on_decommit(size);
        parent.dealloc_range(base, size);
      }
    };
  };
} // namespace snmalloc
