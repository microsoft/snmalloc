#pragma once

#include "../backend/backend.h"

namespace snmalloc
{
  template<
    typename PAL,
    typename Pagemap,
    typename Base,
    size_t MinSizeBits = MinBaseSizeBits<PAL>()>
  struct MetaProtectedRangeLocalState
  {
  private:
    // Global range of memory
    using GlobalR = Pipe<
      Base,
      LargeBuddyRange<24, bits::BITS - 1, Pagemap, MinSizeBits>,
      LogRange<2>,
      GlobalRange<>>;

    // Central source of object-range, does not pass back to GlobalR as
    // that would allow flows from Objects to Meta-data, and thus UAF
    // would be able to corrupt meta-data.
    using CentralObjectRange = Pipe<
      GlobalR,
      LargeBuddyRange<24, bits::BITS - 1, Pagemap>,
      LogRange<3>,
      GlobalRange<>,
      CommitRange<PAL>,
      StatsRange<>>;

    // Centralised source of meta-range
    using CentralMetaRange = Pipe<
      GlobalR,
      SubRange<PAL, 6>, // Use SubRange to introduce guard pages.
      LargeBuddyRange<24, bits::BITS - 1, Pagemap>,
      LogRange<4>,
      GlobalRange<>,
      CommitRange<PAL>,
      StatsRange<>>;

    // Local caching of object range
    using ObjectRange =
      Pipe<CentralObjectRange, LargeBuddyRange<21, 21, Pagemap>, LogRange<5>>;

    // Local caching of meta-data range
    using MetaRange = Pipe<
      CentralMetaRange,
      LargeBuddyRange<21 - 6, bits::BITS - 1, Pagemap>,
      SmallBuddyRange<>>;

  public:
    using Stats = StatsCombiner<CentralObjectRange, CentralMetaRange>;

    ObjectRange object_range;

    MetaRange meta_range;

    MetaRange& get_meta_range()
    {
      return meta_range;
    }

    // Create global range that can service small meta-data requests.
    // Don't want to add the SmallBuddyRange to the CentralMetaRange as that
    // would require committing memory inside the main global lock.
    using GlobalMetaRange =
      Pipe<CentralMetaRange, SmallBuddyRange<>, GlobalRange<>>;
  };
}