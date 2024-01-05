#pragma once

#include "../ds_core/ds_core.h"
#include "localalloc.h"

namespace snmalloc
{
  template<SNMALLOC_CONCEPT(IsConfig) Config>
  inline static void cleanup_unused()
  {
#ifndef SNMALLOC_PASS_THROUGH
    static_assert(
      Config::Options.CoreAllocIsPoolAllocated,
      "Global cleanup is available only for pool-allocated configurations");
    // Call this periodically to free and coalesce memory allocated by
    // allocators that are not currently in use by any thread.
    // One atomic operation to extract the stack, another to restore it.
    // Handling the message queue for each stack is non-atomic.
    auto* first = AllocPool<Config>::extract();
    auto* alloc = first;
    decltype(alloc) last;

    if (alloc != nullptr)
    {
      while (alloc != nullptr)
      {
        alloc->flush();
        last = alloc;
        alloc = AllocPool<Config>::extract(alloc);
      }

      AllocPool<Config>::restore(first, last);
    }
#endif
  }

  /**
    If you pass a pointer to a bool, then it returns whether all the
    allocators are empty. If you don't pass a pointer to a bool, then will
    raise an error all the allocators are not empty.
   */
  template<SNMALLOC_CONCEPT(IsConfig) Config>
  inline static void debug_check_empty(bool* result = nullptr)
  {
#ifndef SNMALLOC_PASS_THROUGH
    static_assert(
      Config::Options.CoreAllocIsPoolAllocated,
      "Global status is available only for pool-allocated configurations");
    // This is a debugging function. It checks that all memory from all
    // allocators has been freed.
    auto* alloc = AllocPool<Config>::iterate();

#  ifdef SNMALLOC_TRACING
    message<1024>("debug check empty: first {}", alloc);
#  endif
    bool done = false;
    bool okay = true;

    while (!done)
    {
#  ifdef SNMALLOC_TRACING
      message<1024>("debug_check_empty: Check all allocators!");
#  endif
      done = true;
      alloc = AllocPool<Config>::iterate();
      okay = true;

      while (alloc != nullptr)
      {
#  ifdef SNMALLOC_TRACING
        message<1024>("debug check empty: {}", alloc);
#  endif
        // Check that the allocator has freed all memory.
        // repeat the loop if empty caused message sends.
        if (alloc->debug_is_empty(&okay))
        {
          done = false;
#  ifdef SNMALLOC_TRACING
          message<1024>("debug check empty: sent messages {}", alloc);
#  endif
        }

#  ifdef SNMALLOC_TRACING
        message<1024>("debug check empty: okay = {}", okay);
#  endif
        alloc = AllocPool<Config>::iterate(alloc);
      }
    }

    if (result == nullptr)
      SNMALLOC_CHECK(RemoteDeallocCache::remote_inflight.get_curr() == 0);

    if (result != nullptr)
    {
      *result = okay;
      return;
    }

    // Redo check so abort is on allocator with allocation left.
    if (!okay)
    {
      alloc = AllocPool<Config>::iterate();
      while (alloc != nullptr)
      {
        alloc->debug_is_empty(nullptr);
        alloc = AllocPool<Config>::iterate(alloc);
      }
    }
#else
    UNUSED(result);
#endif
  }

  template<SNMALLOC_CONCEPT(IsConfig) Config>
  inline static void debug_in_use(size_t count)
  {
    static_assert(
      Config::Options.CoreAllocIsPoolAllocated,
      "Global status is available only for pool-allocated configurations");
    auto alloc = AllocPool<Config>::iterate();
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
      alloc = AllocPool<Config>::iterate(alloc);

      if (count != 0)
      {
        error("Error: two few allocators in use.");
      }
    }
  }

  template<SNMALLOC_CONCEPT(IsConfig) Config>
  inline static void get_stats(AllocStats& stats)
  {
    auto alloc = AllocPool<Config>::iterate();
    while (alloc != nullptr)
    {
      stats += alloc->get_stats();
      alloc = AllocPool<Config>::iterate(alloc);
    }
  }

  template<SNMALLOC_CONCEPT(IsConfig) Config>
  inline static void print_alloc_stats()
  {
    static std::atomic<size_t> dump{0};

    auto l_dump = dump++;
    if (l_dump == 0)
    {
      message<1024>(
        "snmalloc_allocs,dumpid,sizeclass,size,allocated,deallocated,in_use,"
        "bytes,slabs allocated,slabs deallocated,slabs in_use,slabs bytes");
      message<1024>(
        "snmalloc_totals,dumpid,backend bytes,peak backend "
        "bytes,requested,slabs requested bytes,remote inflight bytes,allocator "
        "count");
    }

    AllocStats stats;
    snmalloc::get_stats<Config>(stats);
    size_t total_live{0};
    size_t total_live_slabs{0};
    for (size_t i = 0; i < snmalloc::SIZECLASS_REP_SIZE; i++)
    {
      auto sc = snmalloc::sizeclass_t::from_raw(i);
      auto allocated = *stats[sc].objects_allocated;
      auto deallocated = *stats[sc].objects_deallocated;
      auto slabs_allocated = *stats[sc].slabs_allocated;
      auto slabs_deallocated = *stats[sc].slabs_deallocated;
      if (allocated == 0 && deallocated == 0)
        continue;
      auto size = snmalloc::sizeclass_full_to_size(sc);
      auto slab_size = snmalloc::sizeclass_full_to_slab_size(sc);
      auto in_use = allocated - deallocated;
      auto amount = in_use * size;
      total_live += amount;
      auto in_use_slabs = slabs_allocated - slabs_deallocated;
      auto amount_slabs = in_use_slabs * slab_size;
      total_live_slabs += amount_slabs;

      snmalloc::message<1024>(
        "snmalloc_allocs,{},{},{},{},{},{},{},{},{},{},{}",
        l_dump,
        i,
        size,
        allocated,
        deallocated,
        in_use,
        amount,
        slabs_allocated,
        slabs_deallocated,
        in_use_slabs,
        amount_slabs);
    }
    snmalloc::message<1024>(
      "snmalloc_totals,{},{},{},{},{},{},{}",
      l_dump,
      Config::Backend::get_current_usage(),
      Config::Backend::get_peak_usage(),
      total_live,
      total_live_slabs,
      RemoteDeallocCache::remote_inflight.get_curr(),
      Config::pool().get_count());
  }
} // namespace snmalloc
