#pragma once

#include "../ds/helpers.h"
#include "alloc.h"
#include "pool.h"

namespace snmalloc
{
  inline bool needs_initialisation(void*);
  void* init_thread_allocator(function_ref<void*(void*)>);

  template<class MemoryProvider, class Alloc>
  class AllocPool : Pool<Alloc, MemoryProvider>
  {
    using Parent = Pool<Alloc, MemoryProvider>;

  public:
    static AllocPool* make(MemoryProvider& mp)
    {
      static_assert(
        sizeof(AllocPool) == sizeof(Parent),
        "You cannot add fields to this class.");
      // This cast is safe due to the static assert.
      return static_cast<AllocPool*>(Parent::make(mp));
    }

    static AllocPool* make() noexcept
    {
      return make(default_memory_provider());
    }

    Alloc* acquire()
    {
      return Parent::acquire(Parent::memory_provider);
    }

    void release(Alloc* a)
    {
      Parent::release(a);
    }

  public:
    void aggregate_stats(Stats& stats)
    {
      auto* alloc = Parent::iterate();

      while (alloc != nullptr)
      {
        stats.add(alloc->stats());
        alloc = Parent::iterate(alloc);
      }
    }

#ifdef USE_SNMALLOC_STATS
    void print_all_stats(std::ostream& o, uint64_t dumpid = 0)
    {
      auto alloc = Parent::iterate();

      while (alloc != nullptr)
      {
        alloc->stats().template print<Alloc>(o, dumpid, alloc->id());
        alloc = Parent::iterate(alloc);
      }
    }
#else
    void print_all_stats(void*& o, uint64_t dumpid = 0)
    {
      UNUSED(o);
      UNUSED(dumpid);
    }
#endif

    void cleanup_unused()
    {
#ifndef SNMALLOC_PASS_THROUGH
      // Call this periodically to free and coalesce memory allocated by
      // allocators that are not currently in use by any thread.
      // One atomic operation to extract the stack, another to restore it.
      // Handling the message queue for each stack is non-atomic.
      auto* first = Parent::extract();
      auto* alloc = first;
      decltype(alloc) last;

      if (alloc != nullptr)
      {
        while (alloc != nullptr)
        {
          alloc->handle_message_queue();
          last = alloc;
          alloc = Parent::extract(alloc);
        }

        restore(first, last);
      }
#endif
    }

    /**
      If you pass a pointer to a bool, then it returns whether all the
      allocators are empty. If you don't pass a pointer to a bool, then will
      raise an error all the allocators are not empty.
     */
    void debug_check_empty(bool* result = nullptr)
    {
#ifndef SNMALLOC_PASS_THROUGH
      // This is a debugging function. It checks that all memory from all
      // allocators has been freed.
      auto* alloc = Parent::iterate();

      bool done = false;
      bool okay = true;

      while (!done)
      {
        done = true;
        alloc = Parent::iterate();
        okay = true;

        while (alloc != nullptr)
        {
          // Check that the allocator has freed all memory.
          alloc->debug_is_empty(&okay);

          // Post all remotes, including forwarded ones. If any allocator posts,
          // repeat the loop.
          if (alloc->remote.capacity < REMOTE_CACHE)
          {
            alloc->stats().remote_post();
            alloc->remote.post(alloc->get_trunc_id());
            done = false;
          }

          alloc = Parent::iterate(alloc);
        }
      }

      if (result != nullptr)
      {
        *result = okay;
        return;
      }

      if (!okay)
      {
        alloc = Parent::iterate();
        while (alloc != nullptr)
        {
          alloc->debug_is_empty(nullptr);
          alloc = Parent::iterate(alloc);
        }
      }
#else
      UNUSED(result);
#endif
    }

    void debug_in_use(size_t count)
    {
      auto alloc = Parent::iterate();
      while (alloc != nullptr)
      {
        if (alloc->debug_is_in_use())
        {
          if (count == 0)
          {
            error("ERROR: allocator in use.");
          }
          count--;
        }
        alloc = Parent::iterate(alloc);

        if (count != 0)
        {
          error("Error: two few allocators in use.");
        }
      }
    }
  };

  using Alloc = Allocator<
    needs_initialisation,
    init_thread_allocator,
    GlobalVirtual,
    SNMALLOC_DEFAULT_CHUNKMAP,
    true>;

  inline AllocPool<GlobalVirtual, Alloc>*& current_alloc_pool()
  {
    return Singleton<
      AllocPool<GlobalVirtual, Alloc>*,
      AllocPool<GlobalVirtual, Alloc>::make>::get();
  }

  template<class MemoryProvider, class Alloc>
  inline AllocPool<MemoryProvider, Alloc>* make_alloc_pool(MemoryProvider& mp)
  {
    return AllocPool<MemoryProvider, Alloc>::make(mp);
  }

} // namespace snmalloc
