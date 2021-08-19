#pragma once

#include "../ds/helpers.h"
#include "localalloc.h"

namespace snmalloc
{
  template<class SharedStateHandle>
  inline static void aggregate_stats(Stats& stats)
  {
    static_assert(
      SharedStateHandle::Options.CoreAllocIsPoolAllocated,
      "Global statistics are available only for pool-allocated configurations");
    auto* alloc = AllocPool<CoreAllocator<SharedStateHandle>>::template iterate<
      SharedStateHandle>();

    while (alloc != nullptr)
    {
      auto a = alloc->attached_stats();
      if (a != nullptr)
        stats.add(*a);
      stats.add(alloc->stats());
      alloc = AllocPool<CoreAllocator<SharedStateHandle>>::template iterate<
        SharedStateHandle>(alloc);
    }
  }

#ifdef USE_SNMALLOC_STATS
  template<class SharedStateHandle>
  inline static void print_all_stats(std::ostream& o, uint64_t dumpid = 0)
  {
    static_assert(
      SharedStateHandle::Options.CoreAllocIsPoolAllocated,
      "Global statistics are available only for pool-allocated configurations");
    auto alloc = AllocPool<CoreAllocator<SharedStateHandle>>::template iterate<
      SharedStateHandle>();

    while (alloc != nullptr)
    {
      auto stats = alloc->stats();
      if (stats != nullptr)
        stats->template print<decltype(alloc)>(o, dumpid, alloc->id());
      alloc = AllocPool<CoreAllocator<SharedStateHandle>>::template iterate<
        SharedStateHandle>(alloc);
    }
  }
#else
  template<class SharedStateHandle>
  inline static void print_all_stats(void*& o, uint64_t dumpid = 0)
  {
    UNUSED(o);
    UNUSED(dumpid);
  }
#endif

  template<class SharedStateHandle>
  inline static void cleanup_unused()
  {
#ifndef SNMALLOC_PASS_THROUGH
    static_assert(
      SharedStateHandle::Options.CoreAllocIsPoolAllocated,
      "Global cleanup is available only for pool-allocated configurations");
    // Call this periodically to free and coalesce memory allocated by
    // allocators that are not currently in use by any thread.
    // One atomic operation to extract the stack, another to restore it.
    // Handling the message queue for each stack is non-atomic.
    auto* first = AllocPool<CoreAllocator<SharedStateHandle>>::extract();
    auto* alloc = first;
    decltype(alloc) last;

    if (alloc != nullptr)
    {
      while (alloc != nullptr)
      {
        alloc->flush();
        last = alloc;
        alloc = AllocPool<CoreAllocator<SharedStateHandle>>::extract(alloc);
      }

      AllocPool<CoreAllocator<SharedStateHandle>>::restore(first, last);
    }
#endif
  }

  /**
    If you pass a pointer to a bool, then it returns whether all the
    allocators are empty. If you don't pass a pointer to a bool, then will
    raise an error all the allocators are not empty.
   */
  template<class SharedStateHandle>
  inline static void debug_check_empty(bool* result = nullptr)
  {
#ifndef SNMALLOC_PASS_THROUGH
    static_assert(
      SharedStateHandle::Options.CoreAllocIsPoolAllocated,
      "Global status is available only for pool-allocated configurations");
    // This is a debugging function. It checks that all memory from all
    // allocators has been freed.
    auto* alloc = AllocPool<CoreAllocator<SharedStateHandle>>::template iterate<
      SharedStateHandle>();

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
      alloc = AllocPool<CoreAllocator<SharedStateHandle>>::template iterate<
        SharedStateHandle>();
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
        alloc = AllocPool<CoreAllocator<SharedStateHandle>>::template iterate<
          SharedStateHandle>(alloc);
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
      alloc = AllocPool<CoreAllocator<SharedStateHandle>>::template iterate<
        SharedStateHandle>();
      while (alloc != nullptr)
      {
        alloc->debug_is_empty(nullptr);
        alloc = AllocPool<CoreAllocator<SharedStateHandle>>::template iterate<
          SharedStateHandle>(alloc);
      }
    }
#else
    UNUSED(result);
#endif
  }

  template<class SharedStateHandle>
  inline static void debug_in_use(size_t count)
  {
    static_assert(
      SharedStateHandle::Options.CoreAllocIsPoolAllocated,
      "Global status is available only for pool-allocated configurations");
    auto alloc = AllocPool<CoreAllocator<SharedStateHandle>>::template iterate<
      SharedStateHandle>();
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
      alloc = AllocPool<CoreAllocator<SharedStateHandle>>::template iterate<
        SharedStateHandle>(alloc);

      if (count != 0)
      {
        error("Error: two few allocators in use.");
      }
    }
  }

} // namespace snmalloc
