#pragma once

#include "commonconfig.h"

namespace snmalloc
{
  // Forward reference to thread local cleanup.
  void register_clean_up();

  class Globals : public CommonConfig
  {
    SNMALLOC_REQUIRE_CONSTINIT
    inline static BackendAllocator::GlobalState<Pal, false> backend_state;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static SlabAllocatorState slab_allocator_state;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static PoolState<CoreAlloc<Globals>> alloc_pool;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static std::atomic<bool> initialised{false};

    SNMALLOC_REQUIRE_CONSTINIT
    inline static std::atomic_flag initialisation_lock{};

  public:
    BackendAllocator::GlobalState<Pal, false>& get_backend_state()
    {
      return backend_state;
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
      backend_state.init();

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
