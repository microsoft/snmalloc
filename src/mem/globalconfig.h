#pragma once

#include "commonconfig.h"

namespace snmalloc
{
  // Forward reference to thread local cleanup.
  void register_clean_up();

  class Globals : public CommonConfig
  {
    SNMALLOC_REQUIRE_CONSTINIT
    inline static AddressSpaceManager<Pal> address_space;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static FlatPagemap<MIN_CHUNK_BITS, Meta, Pal, false, &default_entry>
      pagemap;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static SlabAllocatorState slab_allocator_state;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static PoolState<CoreAlloc<Globals>> alloc_pool;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static std::atomic<bool> initialised{false};

    SNMALLOC_REQUIRE_CONSTINIT
    inline static std::atomic_flag initialisation_lock{};

  public:
    AddressSpaceManager<DefaultPal>& get_meta_address_space()
    {
      return address_space;
    }

    AddressSpaceManager<DefaultPal>& get_object_address_space()
    {
      return address_space;
    }

    FlatPagemap<MIN_CHUNK_BITS, Meta, Pal, false, &default_entry>& get_pagemap()
    {
      return pagemap;
    }

    SlabAllocatorState& get_slab_allocator_state()
    {
      return slab_allocator_state;
    }

    PoolState<CoreAlloc<Globals>>& pool()
    {
      return alloc_pool;
    }

    static constexpr bool IsQueueInline = true;

    // Performs initialisation for this configuration
    // of allocators.  Needs to be idempotent,
    // and concurrency safe.
    void ensure_init()
    {
      FlagLock lock{initialisation_lock};
#ifdef SNMALLOC_TRACING
      std::cout << "Run init_impl" << std::endl;
#endif

      if (initialised)
        return;

      // Need to initialise pagemap.
      pagemap.init(&get_meta_address_space());

      initialised = true;
    }

    bool is_initialised()
    {
      return initialised;
    }

    // This needs to be a forward reference as the
    // thread local state will need to know about this.
    // This may allocate, so should only be called once
    // a thread local allocator is available.
    void register_clean_up()
    {
      snmalloc::register_clean_up();
    }

    // This is an empty structure as all the state is global
    // for this allocator configuration.
    static constexpr Globals get_handle()
    {
      return {};
    }
  };
}
