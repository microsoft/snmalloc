#pragma once

#include "../backend/backend.h"

#include "../backend/slaballocator.h"
#include "fastalloc.h"
#include "pool.h"

#include "../ds/defines.h"

namespace snmalloc
{
  // Forward reference to thread local cleanup.
  void register_clean_up();

  struct Globals
  {
    using Meta = MetaEntry;
    using Pal = DefaultPal;

    // SNMALLOC_REQUIRE_CONSTINIT
    inline static Metaslab default_meta_slab;

    /**
     * Special remote that should never be used as a real remote.
     * This is used to initialise allocators that should always hit the
     * remote path for deallocation. Hence moving a branch of the critical
     * path.
     */
    inline static RemoteAllocator unused_remote;

    /**
     * Special remote that is used in meta-data for large allocations.
     *
     * nullptr is considered a large allocations for this purpose to move
     * of the critical path.
     */
    inline static RemoteAllocator fake_large_remote_impl;
    inline static constexpr RemoteAllocator* fake_large_remote{&fake_large_remote_impl};

    /**
     * We use fake_large_remote so that nullptr, will hit the large
     * allocation path which is less performance sensitive.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static MetaEntry default_entry{&default_meta_slab, fake_large_remote};

    SNMALLOC_REQUIRE_CONSTINIT
    inline static AddressSpaceManager<Pal> address_space;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static AddressSpaceManager<Pal> meta_address_space;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static FlatPagemap<MIN_CHUNK_BITS, Meta, false, &default_entry>
      pagemap;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static SlabAllocatorState slab_allocator_state;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static PoolState<CoreAlloc<Globals>> alloc_pool;

    inline static std::atomic<bool> initialised{false};

    AddressSpaceManager<DefaultPal>& get_meta_address_space()
    {
      return meta_address_space;
    }

    AddressSpaceManager<DefaultPal>& get_object_address_space()
    {
      return address_space;
    }

    FlatPagemap<MIN_CHUNK_BITS, Meta, false, &default_entry>& get_pagemap()
    {
      return pagemap;
    }

    SlabAllocatorState& get_slab_allocator_state()
    {
      return slab_allocator_state;
    }

    static PoolState<CoreAlloc<Globals>>& pool()
    {
      return alloc_pool;
    }

    static constexpr bool IsQueueInline = true;

    // Performs initialisation for this configuration
    // of allocators.  Will be called at most once
    // before any other datastructures are accessed.
    static int init() noexcept
    {
#ifdef SNMALLOC_TRACING
      std::cout << "Run init_impl" << std::endl;
#endif
      // Need to initialise pagemap.
      pagemap.init(&meta_address_space);

      // The nullptr should contain the default value.
      // This will make alloc_size of nullptr return 0
      // as required.
      pagemap.add(0, default_entry);

      initialised = true;
      return 0;
    }

    static bool is_initialised()
    {
      return initialised;
    }

    // This needs to be a forward reference as the
    // thread local state will need to know about this.
    static void register_clean_up()
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

using Alloc = snmalloc::FastAllocator<snmalloc::Globals>;