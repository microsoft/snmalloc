

#pragma once

#include "../backend/backend.h"

namespace snmalloc
{
  /**
   * Default configuration that does not provide any meta-data protection.
   * 
   * PAL is the underlying PAL that is used to Commit memory ranges.
   * 
   * Base is where memory is sourced from.
   * 
   * MinSizeBits is the minimum request size that can be passed to Base.
   * On Windows this 16 as VirtualAlloc cannot reserve less than 64KiB.
   * Alternative configurations might make this 2MiB so that huge pages
   * can be used.
   */
  template<
    typename PAL,
    typename Pagemap,
    typename Base = EmptyRange,
    size_t MinSizeBits = MinBaseSizeBits<PAL>()>
  struct StandardLocalState
  {
    // Global range of memory, expose this so can be filled by init.
    using GlobalR = Pipe<
      Base,
      LargeBuddyRange<24, bits::BITS - 1, Pagemap, MinSizeBits>,
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

    // Where we get user allocations from.
    ObjectRange object_range;

    // Where we get meta-data allocations from.
    ObjectRange& get_meta_range()
    {
      // Use the object range to service meta-data requests.
      return object_range;
    }
  };
} // namespace snmalloc