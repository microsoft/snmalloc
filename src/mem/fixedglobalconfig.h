#pragma once

#include "../backend/backend.h"
#include "../mem/corealloc.h"
#include "../mem/pool.h"
#include "../mem/slaballocator.h"
#include "commonconfig.h"

namespace snmalloc
{
  /**
   * A single fixed address range allocator configuration
   */
  class FixedGlobals : public CommonConfig
  {
  public:
    using Backend = BackendAllocator<PALNoAlloc<Pal>, true>;

  private:
    inline static Backend::GlobalState backend_state;

    inline static ChunkAllocatorState slab_allocator_state;

    inline static PoolState<CoreAlloc<FixedGlobals>> alloc_pool;

  public:
    static Backend::GlobalState& get_backend_state()
    {
      return backend_state;
    }

    ChunkAllocatorState& get_slab_allocator_state()
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
      get_backend_state().init(base, length);
    }

    constexpr static FixedGlobals get_handle()
    {
      return {};
    }
  };
}
