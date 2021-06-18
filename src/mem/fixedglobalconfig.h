#pragma once

#include "commonconfig.h"

namespace snmalloc
{
    /**
     * A single fixed address range allocator configuration
     */
    class FixedGlobals : public CommonConfig
    {
    inline static AddressSpaceManager<PALNoAlloc<DefaultPal>> address_space;

    inline static FlatPagemap<
        MIN_CHUNK_BITS,
        CommonConfig::Meta,
        Pal,
        true,
        &CommonConfig::default_entry>
        pagemap;

    inline static SlabAllocatorState slab_allocator_state;

    inline static PoolState<CoreAlloc<FixedGlobals>> alloc_pool;

    public:
    AddressSpaceManager<PALNoAlloc<DefaultPal>>& get_meta_address_space()
    {
        return address_space;
    }

    AddressSpaceManager<PALNoAlloc<DefaultPal>>& get_object_address_space()
    {
        return address_space;
    }

    FlatPagemap<
        MIN_CHUNK_BITS,
        CommonConfig::Meta,
        Pal,
        true,
        &CommonConfig::default_entry>&
    get_pagemap()
    {
        return pagemap;
    }

    SlabAllocatorState& get_slab_allocator_state()
    {
        return slab_allocator_state;
    }

    PoolState<CoreAlloc<FixedGlobals>>& pool()
    {
        return alloc_pool;
    }

    static constexpr bool IsQueueInline = true;

    // Performs initialisation for this configuration
    // of allocators.  Will be called at most once
    // before any other datastructures are accessed.
    void ensure_init() noexcept
    {
    #ifdef SNMALLOC_TRACING
        std::cout << "Run init_impl" << std::endl;
    #endif
    }

    static bool is_initialised()
    {
        return true;
    }

    // This needs to be a forward reference as the
    // thread local state will need to know about this.
    // This may allocate, so must be called once a thread
    // local allocator exists.
    static void register_clean_up()
    {
        snmalloc::register_clean_up();
    }

    static void init(CapPtr<void, CBChunk> base, size_t length)
    {
        address_space.add_range(base, length);
        pagemap.init(&address_space, address_cast(base), length);
    }

    constexpr static FixedGlobals get_handle()
    {
        return {};
    }
  };
}
