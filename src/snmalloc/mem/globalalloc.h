#pragma once

#include "../ds_core/ds_core.h"
#include "localalloc.h"

namespace snmalloc
{
  template<SNMALLOC_CONCEPT(ConceptBackendGlobals) SharedStateHandle>
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
    auto* first = AllocPool<SharedStateHandle>::extract();
    auto* alloc = first;
    decltype(alloc) last;

    if (alloc != nullptr)
    {
      while (alloc != nullptr)
      {
        alloc->flush();
        last = alloc;
        alloc = AllocPool<SharedStateHandle>::extract(alloc);
      }

      AllocPool<SharedStateHandle>::restore(first, last);
    }
#endif
  }

  /**
    If you pass a pointer to a bool, then it returns whether all the
    allocators are empty. If you don't pass a pointer to a bool, then will
    raise an error all the allocators are not empty.
   */
  template<SNMALLOC_CONCEPT(ConceptBackendGlobals) SharedStateHandle>
  inline static void debug_check_empty(bool* result = nullptr)
  {
#ifndef SNMALLOC_PASS_THROUGH
    static_assert(
      SharedStateHandle::Options.CoreAllocIsPoolAllocated,
      "Global status is available only for pool-allocated configurations");
    // This is a debugging function. It checks that all memory from all
    // allocators has been freed.
    auto* alloc = AllocPool<SharedStateHandle>::iterate();

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
      alloc = AllocPool<SharedStateHandle>::iterate();
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
        alloc = AllocPool<SharedStateHandle>::iterate(alloc);
      }
    }

    if (result == nullptr  && RemoteDeallocCache::remote_inflight.get_curr() != 0)
      error("ERROR: RemoteDeallocCache::remote_inflight != 0");

    if (result != nullptr)
    {
      *result = okay;
      return;
    }

    // Redo check so abort is on allocator with allocation left.
    if (!okay)
    {
      alloc = AllocPool<SharedStateHandle>::iterate();
      while (alloc != nullptr)
      {
        alloc->debug_is_empty(nullptr);
        alloc = AllocPool<SharedStateHandle>::iterate(alloc);
      }
    }
#else
    UNUSED(result);
#endif
  }

  template<SNMALLOC_CONCEPT(ConceptBackendGlobals) SharedStateHandle>
  inline static void debug_in_use(size_t count)
  {
    static_assert(
      SharedStateHandle::Options.CoreAllocIsPoolAllocated,
      "Global status is available only for pool-allocated configurations");
    auto alloc = AllocPool<SharedStateHandle>::iterate();
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
      alloc = AllocPool<SharedStateHandle>::iterate(alloc);

      if (count != 0)
      {
        error("Error: two few allocators in use.");
      }
    }
  }

  template<SNMALLOC_CONCEPT(ConceptBackendGlobals) SharedStateHandle>
  inline static AllocStats get_stats()
  {
    auto alloc = AllocPool<SharedStateHandle>::iterate();
    AllocStats stats;
    while (alloc != nullptr)
    {
      stats += alloc->get_stats();
      alloc = AllocPool<SharedStateHandle>::iterate(alloc);
    }
    return stats;
  }

  template<SNMALLOC_CONCEPT(ConceptBackendGlobals) SharedStateHandle>
  inline static void print_alloc_stats()
  {
#ifndef SNMALLOC_PASS_THROUGH // This test depends on snmalloc internals
    auto stats = snmalloc::get_stats<SharedStateHandle>();
    for (size_t i = 0; i < snmalloc::SIZECLASS_REP_SIZE; i++)
    {
      auto sc = snmalloc::sizeclass_t::from_raw(i);
      auto allocated = *stats[sc].objects_allocated;
      auto deallocated = *stats[sc].objects_deallocated;
      if (allocated == 0 && deallocated == 0)
        continue;
      auto size =
        snmalloc::sizeclass_full_to_size(snmalloc::sizeclass_t::from_raw(i));
      auto in_use = allocated - deallocated;
      snmalloc::message<1024>("SNMALLOCallocs,{},{},{},{},{}", i, size, allocated, deallocated, in_use);
    }
#endif
  }
} // namespace snmalloc
