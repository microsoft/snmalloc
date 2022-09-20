#include "test/setup.h"

#include <iostream>
#include <snmalloc/backend/fixedglobalconfig.h>
#include <snmalloc/backend/standard_range.h>
#include <snmalloc/backend_helpers/backend_helpers.h>
#include <snmalloc/snmalloc.h>

#ifdef assert
#  undef assert
#endif
#define assert please_use_SNMALLOC_ASSERT

using namespace snmalloc;

/**
 * A single fixed address range allocator configuration
 */
template<SNMALLOC_CONCEPT(IsPAL) PAL = DefaultPal>
class MultiRegionConfig final : public CommonConfig
{
public:
  using PagemapEntry = DefaultPagemapEntry;

private:
  using ConcretePagemap = FlatPagemap<MIN_CHUNK_BITS, PagemapEntry, PAL, false>;

  using Pagemap = BasicPagemap<PAL, ConcretePagemap, PagemapEntry, false>;

  static inline FlagWord pagemap_init_lock{};

public:
  class LocalState
  {
  public:
    using ObjectRange = Pipe<
      EmptyRange<>,
      LargeBuddyRange<bits::BITS - 1, bits::BITS - 1, Pagemap>,
      SmallBuddyRange>;

    // Dummy impl to keep concept happy.
    using Stats = Pipe<EmptyRange<>, StatsRange>;

  private:
    ObjectRange object_range;

    void ensure_pagemap_init()
    {
      auto& pagemap = Pagemap::concretePagemap;
      if (pagemap.is_initialised())
        return;

      FlagLock lock(pagemap_init_lock);

      if (pagemap.is_initialised())
        return;

      pagemap.init();
    }

  public:
    // This should not be called.
    using GlobalMetaRange = EmptyRange<>;

    // Where we get user allocations from.
    ObjectRange* get_object_range()
    {
      return &object_range;
    }

    // Where we get meta-data allocations from.
    ObjectRange& get_meta_range()
    {
      // Use the object range to service meta-data requests.
      return object_range;
    }

    LocalState(void* base, size_t size) : object_range()
    {
      // Ensure the communal pagemap is initialised.
      ensure_pagemap_init();

      // Notify that pagemap requires committed memory for this range.
      Pagemap::register_range(address_cast(base), size);

      // Fill the range owned by this region with memory.
      object_range.dealloc_range(capptr::Arena<void>::unsafe_from(base), size);
    }
  };

  using Backend = BackendAllocator<PAL, PagemapEntry, Pagemap, LocalState>;
  using Pal = PAL;

private:
public:
  constexpr static snmalloc::Flags Options{
    .IsQueueInline = true,
    .CoreAllocOwnsLocalState = false,
    .CoreAllocIsPoolAllocated = false,
    .LocalAllocSupportsLazyInit = false,
    .QueueHeadsAreTame = true,
    .HasDomesticate = false,
  };

  static void register_clean_up() {}
};

using CustomConfig = MultiRegionConfig<DefaultPal>;
using FixedAlloc = LocalAllocator<CustomConfig>;
using CoreAlloc = CoreAllocator<CustomConfig>;

class Region
{
public:
  FixedAlloc alloc;

private:
  CustomConfig::LocalState region_state;

  CoreAlloc core_alloc;

public:
  Region(void* base, size_t size)
  : region_state(base, size),
    core_alloc(&alloc.get_local_cache(), &region_state)
  {
    // Bind the core_alloc into the region local allocator
    alloc.init(&core_alloc);
  }
};

int main()
{
#ifndef SNMALLOC_PASS_THROUGH // Depends on snmalloc specific features
  setup();

  // 28 is large enough to produce a nested allocator.
  // It is also large enough for the example to run in.
  // For 1MiB superslabs, SUPERSLAB_BITS + 4 is not big enough for the example.
  auto size = bits::one_at_bit(28);
  auto base = DefaultPal::reserve(size);
  DefaultPal::notify_using<NoZero>(base, size);
  auto end = pointer_offset(base, size);
  std::cout << "Allocated region " << base << " - "
            << pointer_offset(base, size) << std::endl;

  Region r(base, size);
  auto& a = r.alloc;

  size_t object_size = 128;
  size_t count = 0;
  size_t i = 0;
  while (true)
  {
    auto r1 = a.alloc(object_size);
    count += object_size;
    i++;

    if (i == 1024)
    {
      i = 0;
      std::cout << ".";
    }
    // Run until we exhaust the fixed region.
    // This should return null.
    if (r1 == nullptr)
      break;

    if (base > r1)
    {
      std::cout << "Allocated: " << r1 << std::endl;
      abort();
    }
    if (end < r1)
    {
      std::cout << "Allocated: " << r1 << std::endl;
      abort();
    }
  }

  std::cout << "Total allocated: " << count << " out of " << size << std::endl;
  std::cout << "Overhead: 1/" << (double)size / (double)(size - count)
            << std::endl;

  a.teardown();
#endif
}
