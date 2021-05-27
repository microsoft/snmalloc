#pragma once

#ifdef _MSC_VER
#  define ALLOCATOR __declspec(allocator)
#else
#  define ALLOCATOR
#endif

#include "../ds/ptrwrap.h"
#include "fastcache.h"
#include "freelist.h"
#include "corealloc.h"
#include "pool.h"
#include "remoteallocator.h"
#include "sizeclasstable.h"

#include <string.h>
#include <utility>

namespace snmalloc
{
  // This class contains the fastest path code for the allocator.
  template<class SharedStateHandle>
  class FastAllocator
  {
    using CoreAlloc = CoreAlloc<SharedStateHandle>;
    inline static RemoteAllocator unused_remote;

  private:
    // Free list per small size class.  These are used for
    // allocation on the fast path. This part of the code is inspired by
    // mimalloc.
    FastCache small_cache;

    // Amount of bytes before we must send the remote
    // deallocation batch.
    int64_t capacity{0};

    // Underlying allocator for most non-fast path operations.
    CoreAlloc* core_alloc{nullptr};

    // Pointer to the remote allocator message_queue, used to check
    // if a deallocation is local.
    RemoteAllocator* remote_allocator{&unused_remote};

    // As allocation and deallocation can occur during thread teardown
    // we need to record if we are already in that state as we will not
    // receive another teardown call, so each operation needs to release
    // the underlying data structures after the call.
    bool post_teardown{false};

    /**
     * Contains a way to access all the shared state for this allocator.
     * This may have no dynamic state, and be purely static.
     */
    SharedStateHandle handle;

    /**
     * Checks if the core allocator has been initialised, and runs the
     * `action` with the arguments, args.
     *
     * If the core allocator is not initialised, then first initialise it,
     * and then perform the action using the core allocator.
     *
     * This is an abstraction of the common pattern of check initialisation,
     * and then performing the operations.  It is carefully crafted to tail
     * call the continuations, and thus generate good code for the fast path.
     */
    template<typename Action, typename... Args>
    SNMALLOC_FAST_PATH decltype(auto) check_init(Action action, Args... args)
    {
      if (likely(core_alloc != nullptr))
      {
        return action(core_alloc, args...);
      }
      return lazy_init(action, args...);
    }

    /**
     * This initialises the fast allocator by acquiring a core allocator, and
     * setting up its local copy of data structures.
     */
    template<typename Action, typename... Args>
    SNMALLOC_SLOW_PATH decltype(auto) lazy_init(Action action, Args... args)
    {
      SNMALLOC_ASSERT(core_alloc == nullptr);

      Singleton<int, SharedStateHandle::init>::get();

      init();

      // register_clean_up must be called after init.  register clean up may be
      // implemented with allocation, so need to ensure we have a valid
      // allocator at this point.
      if (!post_teardown)
        // TODO: Should this be a singleton, so we only call it once?
        SharedStateHandle::register_clean_up();

      // Perform underlying operation
      auto r = action(core_alloc, args...);

      // After performing underlying operation, in the case of teardown already
      // having begun, we must flush any state we just acquired.
      if (post_teardown)
      {
        // We didn't have an allocator because the thread is being torndown.
        // We need to return any local state, so we don't leak it.
        flush();
      }
      return r;
    }

    /**
     * Allocation that are larger than are handled by the fast allocator must be
     * passed to the core allocator.
     */
    template<ZeroMem zero_mem = NoZero>
    SNMALLOC_SLOW_PATH void* alloc_not_small(size_t size)
    {
      // TODO
      UNUSED(size);
      return nullptr;
      // return check_init(
      //   [](CoreAlloc* core_alloc, size_t size) {
      //     return core_alloc->template alloc<zero_mem>(size);
      //   },
      //   size);
    }

    template<ZeroMem zero_mem>
    SNMALLOC_FAST_PATH void* small_alloc(size_t size)
    {
// /      SNMALLOC_ASSUME(size <= SLAB_SIZE);
      auto slowpath = [&](
                        sizeclass_t sizeclass,
                        FreeListIter* fl) SNMALLOC_FAST_PATH {
        return check_init(
          //  Note:  FreeListIter& for fl would be nice, but codegen gets
          //  upset in clang.
          [&](CoreAlloc* core_alloc, sizeclass_t sizeclass, FreeListIter* fl) {
            // Setting up the message queue can cause a free list to be
            // populated, so need to check that initialisation hasn't caused
            // that.  Aggressive inlining will remove this.
            if (fl->empty())
              return core_alloc->template small_alloc<zero_mem>(sizeclass, *fl);

            auto r = capptr_reveal(
              capptr_export(fl->take(small_cache.entropy).as_void()));

            return FastCache::zeroing_wrapper(r, sizeclass_to_size(sizeclass));
          },
          sizeclass,
          fl);
      };

      return small_cache.template alloc<zero_mem>(size, slowpath);
    }

  public:
    constexpr FastAllocator()
    {
      handle = SharedStateHandle::get_handle();
    };

    // This is effectively the constructor for the FastAllocator, but due to
    // not wanting initialisation checks on the fast path, it is initialised
    // lazily.
    void init()
    {
      // Should only be called if the allocator has not been initialised.
      SNMALLOC_ASSERT(core_alloc == nullptr);

      // Grab an allocator for this thread.
      core_alloc =
        Pool<CoreAlloc>::acquire(handle, &(this->small_cache), handle);
      core_alloc->attached_cache = &(this->small_cache);

      small_cache.entropy = core_alloc->entropy;
      // small_cache.stats.start();

      // TODO: setup remote allocator
    }

    // Return all state in the fast allocator and release the underlying
    // core allocator.  This is used during teardown to empty the thread
    // local state.
    void flush()
    {
      // Detached thread local state from allocator.
      if (core_alloc != nullptr)
      {
        small_cache.flush([&](auto p) { dealloc(p); });

        // core_alloc->stats().add(small_cache.stats);
        // // Reset stats, required to deal with repeated flushing.
        // new (&small_cache.stats) Stats();

        core_alloc->attached_cache = nullptr;
        // Return underlying allocator to the system.
        Pool<CoreAlloc>::release(handle, core_alloc);

        core_alloc = nullptr;
        remote_allocator = nullptr;
      }
    }

    /**
     * Allocate memory of a dynamically known size.
     */
    template<ZeroMem zero_mem = NoZero>
    SNMALLOC_FAST_PATH ALLOCATOR void* alloc(size_t size)
    {
#ifdef SNMALLOC_PASS_THROUGH
      // snmalloc guarantees a lot of alignment, so we can depend on this
      // make pass through call aligned_alloc with the alignment snmalloc
      // would guarantee.
      void* result = external_alloc::aligned_alloc(
        natural_alignment(size), round_size(size));
      if constexpr (zero_mem == YesZero)
        memset(result, 0, size);
      return result;
#else
      // Perform the - 1 on size, so that zero wraps around and ends up on
      // slow path.
      if (likely((size - 1) <= (sizeclass_to_size(NUM_SIZECLASSES - 1) - 1)))
      {
        // Allocations smaller than the slab size are more likely. Improve
        // branch prediction by placing this case first.
        return small_alloc<zero_mem>(size);
      }

      // TODO capptr_reveal?
      return alloc_not_small<zero_mem>(size);
#endif
    }

    /**
     * Allocate memory of a statically known size.
     */
    template<size_t size, ZeroMem zero_mem = NoZero>
    SNMALLOC_FAST_PATH ALLOCATOR void* alloc()
    {
      // TODO optimise
      return alloc<zero_mem>(size);
    }

    SNMALLOC_FAST_PATH void dealloc(void* p)
    {
      // MetaEntry entry = BackendAllocator::get_meta_data(handle, p);
      // if (remote == entry.remote)
        core_alloc->dealloc_local_object(p);
      // else
      //   dealloc_non_local(p);
    }
      
    // SNMALLOC_FAST_PATH void dealloc_non_local(void* p)
    // {
    //   if (remote == Unit)
    //   {

    //   }
    //   else if (entry.remote == LargeRemote)
    //   {
    //     // TODO
    //   }
    //   else
    //   {

    //   }

    //   // check_init([p](CoreAlloc* core_alloc) -> void* {
    //   //   core_alloc->dealloc(p);
    //   //   return nullptr;
    //   // });
    // }

    SNMALLOC_FAST_PATH void dealloc(void* p, size_t s)
    {
      UNUSED(s);
      dealloc(p);
    }

    template<size_t size>
    SNMALLOC_FAST_PATH void dealloc(void* p)
    {
      UNUSED(size);
      dealloc(p);
    }

    void teardown()
    {
      post_teardown = true;
      if (core_alloc != nullptr)
      {
        flush();
      }
    }

    SNMALLOC_FAST_PATH size_t alloc_size(const void* p_raw)
    {
      UNUSED(p_raw);
      return 0;

      // return check_init(
      //   [](CoreAlloc* core_alloc, const void* p_raw) {
      //     return core_alloc->alloc_size(p_raw);
      //   },
      //   p_raw);
    }

    // template<Boundary location = Start>
    // void* external_pointer(void* p_raw)
    // {
    //   return nullptr;
    //   // // TODO, can we optimise to not need initialisation?
    //   // return check_init(
    //   //   [](CoreAlloc* core_alloc, void* p_raw) {
    //   //     return core_alloc->template external_pointer<location>(p_raw);
    //   //   },
    //   //   p_raw);
    // }
  };

  void register_clean_up();
} // namespace snmalloc