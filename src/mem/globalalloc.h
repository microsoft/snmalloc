#pragma once

#include "../ds/helpers.h"
#include "localalloc.h"

namespace snmalloc
{
  template<class SharedStateHandle>
  inline static void aggregate_stats(SharedStateHandle handle, Stats& stats)
  {
    auto* alloc = Pool<CoreAllocator<SharedStateHandle>>::iterate(handle);

    while (alloc != nullptr)
    {
      auto a = alloc->attached_stats();
      if (a != nullptr)
        stats.add(*a);
      stats.add(alloc->stats());
      alloc = Pool<CoreAllocator<SharedStateHandle>>::iterate(handle, alloc);
    }
  }

#ifdef USE_SNMALLOC_STATS
  template<class SharedStateHandle>
  inline static void print_all_stats(
    SharedStateHandle handle, std::ostream& o, uint64_t dumpid = 0)
  {
    auto alloc = Pool<CoreAllocator<SharedStateHandle>>::iterate(handle);

    while (alloc != nullptr)
    {
      auto stats = alloc->stats();
      if (stats != nullptr)
        stats->template print<Alloc>(o, dumpid, alloc->id());
      alloc = Pool<CoreAllocator<SharedStateHandle>>::iterate(handle, alloc);
    }
  }
#else
  template<class SharedStateHandle>
  inline static void
  print_all_stats(SharedStateHandle handle, void*& o, uint64_t dumpid = 0)
  {
    UNUSED(o);
    UNUSED(dumpid);
    UNUSED(handle);
  }
#endif

  template<class SharedStateHandle>
  inline static void cleanup_unused(SharedStateHandle handle)
  {
#ifndef SNMALLOC_PASS_THROUGH
    // Call this periodically to free and coalesce memory allocated by
    // allocators that are not currently in use by any thread.
    // One atomic operation to extract the stack, another to restore it.
    // Handling the message queue for each stack is non-atomic.
    auto* first = Pool<CoreAllocator<SharedStateHandle>>::extract(handle);
    auto* alloc = first;
    decltype(alloc) last;

    if (alloc != nullptr)
    {
      while (alloc != nullptr)
      {
        alloc->flush();
        last = alloc;
        alloc = Pool<CoreAllocator<SharedStateHandle>>::extract(handle, alloc);
      }

      Pool<CoreAllocator<SharedStateHandle>>::restore(handle, first, last);
    }
#endif
  }

  /**
    If you pass a pointer to a bool, then it returns whether all the
    allocators are empty. If you don't pass a pointer to a bool, then will
    raise an error all the allocators are not empty.
   */
  template<class SharedStateHandle>
  inline static void
  debug_check_empty(SharedStateHandle handle, bool* result = nullptr)
  {
#ifndef SNMALLOC_PASS_THROUGH
    // This is a debugging function. It checks that all memory from all
    // allocators has been freed.
    auto* alloc = Pool<CoreAllocator<SharedStateHandle>>::iterate(handle);

#  ifdef SNMALLOC_TRACING
    std::cout << "debug check empty: first " << alloc << std::endl;
#  endif
    bool done = false;
    bool okay = true;

    while (!done)
    {
#  ifdef SNMALLOC_TRACING
      std::cout << "debug_check_empty: Check all allocators!" << std::endl;
#  endif
      done = true;
      alloc = Pool<CoreAllocator<SharedStateHandle>>::iterate(handle);
      okay = true;

      while (alloc != nullptr)
      {
#  ifdef SNMALLOC_TRACING
        std::cout << "debug check empty: " << alloc << std::endl;
#  endif
        // Check that the allocator has freed all memory.
        // repeat the loop if empty caused message sends.
        if (alloc->debug_is_empty(&okay))
        {
          done = false;
#  ifdef SNMALLOC_TRACING
          std::cout << "debug check empty: sent messages " << alloc
                    << std::endl;
#  endif
        }

#  ifdef SNMALLOC_TRACING
        std::cout << "debug check empty: okay = " << okay << std::endl;
#  endif
        alloc = Pool<CoreAllocator<SharedStateHandle>>::iterate(handle, alloc);
      }
    }

    if (result != nullptr)
    {
      *result = okay;
      return;
    }

    // Redo check so abort is on allocator with allocation left.
    if (!okay)
    {
      alloc = Pool<CoreAllocator<SharedStateHandle>>::iterate(handle);
      while (alloc != nullptr)
      {
        alloc->debug_is_empty(nullptr);
        alloc = Pool<CoreAllocator<SharedStateHandle>>::iterate(handle, alloc);
      }
    }
#else
    UNUSED(result);
#endif
  }

  template<class SharedStateHandle>
  inline static void debug_in_use(SharedStateHandle handle, size_t count)
  {
    auto alloc = Pool<CoreAllocator<SharedStateHandle>>::iterate(handle);
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
      alloc = Pool<CoreAllocator<SharedStateHandle>>::iterate(handle, alloc);

      if (count != 0)
      {
        error("Error: two few allocators in use.");
      }
    }
  }

} // namespace snmalloc
