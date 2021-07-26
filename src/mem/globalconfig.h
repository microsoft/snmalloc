#pragma once

#include "../backend/backend.h"
#include "../mem/corealloc.h"
#include "../mem/pool.h"
#include "../mem/slaballocator.h"
#include "commonconfig.h"

#include <iostream>

namespace snmalloc
{
  // Forward reference to thread local cleanup.
  void register_clean_up();

#ifdef USE_SNMALLOC_STATS
  inline static void print_stats()
  {
    printf("No Stats yet!");
    // Stats s;
    // current_alloc_pool()->aggregate_stats(s);
    // s.print<Alloc>(std::cout);
  }
#endif

  class Globals : public CommonConfig
  {
  public:
    using Backend = BackendAllocator<Pal, false>;

  private:
    SNMALLOC_REQUIRE_CONSTINIT
    inline static Backend::GlobalState backend_state;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static ChunkAllocatorState slab_allocator_state;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static PoolState<CoreAllocator<Globals>> alloc_pool;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static std::atomic<bool> initialised{false};

    SNMALLOC_REQUIRE_CONSTINIT
    inline static std::atomic_flag initialisation_lock{};

  public:
    Backend::GlobalState& get_backend_state()
    {
      return backend_state;
    }

    ChunkAllocatorState& get_slab_allocator_state()
    {
      return slab_allocator_state;
    }

    PoolState<CoreAllocator<Globals>>& pool()
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

      LocalEntropy entropy;
      entropy.init<Pal>();
      // Initialise key for remote deallocation lists
      key_global = FreeListKey(entropy.get_free_list_key());

      // Need to initialise pagemap.
      backend_state.init();

#ifdef USE_SNMALLOC_STATS
      atexit(snmalloc::print_stats);
#endif

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
} // namespace snmalloc
