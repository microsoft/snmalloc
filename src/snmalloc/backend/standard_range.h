

#pragma once

#include "../backend/backend.h"

namespace snmalloc
{
  template<typename PAL, typename Pagemap, typename Base = EmptyRange>
  struct StandardLocalState
  {
    // Global range of memory, expose this so can be filled by init.
    using GlobalR = Pipe<
      Base,
      LargeBuddyRange<24, bits::BITS - 1, Pagemap>,
      LogRange<2>,
      GlobalRange<>>;

    // Track stats of the committed memory
    using Stats = Pipe<GlobalR, CommitRange<PAL>, StatsRange<>>;

  private:
    // Source for object allocations and metadata
    // Use buddy allocators to cache locally.
    using ObjectRange =
      Pipe<Stats, LargeBuddyRange<21, 21, Pagemap>, SmallBuddyRange<>>;

  public:
    // Expose a global range for the initial allocation of meta-data.
    using GlobalMetaRange = Pipe<ObjectRange, GlobalRange<>>;

    ObjectRange object_range;

    ObjectRange& get_meta_range()
    {
      return object_range;
    }
  };
}