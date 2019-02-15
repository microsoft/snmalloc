#pragma once

#include "../ds/flaglock.h"
#include "../ds/helpers.h"
#include "../ds/mpmcstack.h"
#include "../pal/pal.h"
#include "allocstats.h"
#include "baseslab.h"
#include "sizeclass.h"

#include <utility>

namespace snmalloc
{
  class Largeslab : public Baseslab
  {
    // This is the view of a contiguous memory area when it is being kept
    // in the global size-classed caches of available contiguous memory areas.
  private:
    template<class a, Construction c>
    friend class MPMCStack;
    std::atomic<Largeslab*> next;

  public:
    void init()
    {
      kind = Large;
    }
  };

  // This represents the state that the large allcoator needs to add to the
  // global state of the allocator.  This is currently stored in the memory
  // provider, so we add this in.
  template<class MemoryProviderState>
  class MemoryProviderStateMixin : public MemoryProviderState
  {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    size_t bump;
    size_t remaining;

    std::pair<void*, size_t> reserve_block() noexcept
    {
      size_t size = SUPERSLAB_SIZE;
      void* r = ((MemoryProviderState*)this)
                  ->template reserve<false>(&size, SUPERSLAB_SIZE);

      if (size < SUPERSLAB_SIZE)
        error("out of memory");

      ((MemoryProviderState*)this)
        ->template notify_using<NoZero>(r, OS_PAGE_SIZE);
      return std::make_pair(r, size);
    }

    void new_block()
    {
      auto r_size = reserve_block();
      bump = (size_t)r_size.first;
      remaining = r_size.second;
    }

  public:
    /**
     * Stack of large allocations that have been returned for reuse.
     */
    ModArray<NUM_LARGE_CLASSES, MPMCStack<Largeslab, PreZeroed>> large_stack;

    /**
     * Primitive allocator for structure that are required before
     * the allocator can be running.
     ***/
    template<size_t alignment = 64>
    void* alloc_chunk(size_t size)
    {
      // Cache line align
      size = bits::align_up(size, 64);

      void* p;
      {
        FlagLock f(lock);

        auto aligned_bump = bits::align_up(bump, alignment);
        if ((aligned_bump - bump) > remaining)
        {
          new_block();
        }
        else
        {
          remaining -= aligned_bump - bump;
          bump = aligned_bump;
        }

        if (remaining < size)
        {
          new_block();
        }

        p = (void*)bump;
        bump += size;
        remaining -= size;
      }

      auto page_start = bits::align_down((size_t)p, OS_PAGE_SIZE);
      auto page_end = bits::align_up((size_t)p + size, OS_PAGE_SIZE);

      ((MemoryProviderState*)this)
        ->template notify_using<NoZero>(
          (void*)page_start, page_end - page_start);

      return p;
    }
  };

  using Stats = AllocStats<NUM_SIZECLASSES, NUM_LARGE_CLASSES>;

  enum AllowReserve
  {
    NoReserve,
    YesReserve
  };

  template<class MemoryProvider>
  class LargeAlloc
  {
    void* reserved_start = nullptr;
    void* reserved_end = nullptr;

  public:
    // This will be a zero-size structure if stats are not enabled.
    Stats stats;

    MemoryProvider& memory_provider;

    LargeAlloc(MemoryProvider& mp) : memory_provider(mp) {}

    template<AllowReserve allow_reserve>
    bool reserve_memory(size_t need, size_t add)
    {
      if (((size_t)reserved_start + need) > (size_t)reserved_end)
      {
        if (allow_reserve == YesReserve)
        {
          stats.segment_create();
          reserved_start =
            memory_provider.template reserve<false>(&add, SUPERSLAB_SIZE);
          reserved_end = (void*)((size_t)reserved_start + add);
          reserved_start =
            (void*)bits::align_up((size_t)reserved_start, SUPERSLAB_SIZE);

          if (add < need)
            return false;
        }
        else
        {
          return false;
        }
      }

      return true;
    }

    template<ZeroMem zero_mem = NoZero, AllowReserve allow_reserve = YesReserve>
    void* alloc(size_t large_class, size_t size)
    {
      size_t rsize = ((size_t)1 << SUPERSLAB_BITS) << large_class;
      if (size == 0)
        size = rsize;

      void* p = memory_provider.large_stack[large_class].pop();

      if (p == nullptr)
      {
        assert(reserved_start <= reserved_end);
        size_t add;

        if ((rsize + SUPERSLAB_SIZE) < RESERVE_SIZE)
          add = RESERVE_SIZE;
        else
          add = rsize + SUPERSLAB_SIZE;

        if (!reserve_memory<allow_reserve>(rsize, add))
          return nullptr;

        p = (void*)reserved_start;
        reserved_start = (void*)((size_t)p + rsize);

        // All memory is zeroed since it comes from reserved space.
        memory_provider.template notify_using<NoZero>(p, size);
      }
      else
      {
        if ((decommit_strategy != DecommitNone) || (large_class > 0))
        {
          // The first page is already in "use" for the stack element,
          // this will need zeroing for a YesZero call.
          if (zero_mem == YesZero)
            memory_provider.template zero<true>(p, OS_PAGE_SIZE);

          // Notify we are using the rest of the allocation.
          // Passing zero_mem ensures the PAL provides zeroed pages if required.
          memory_provider.template notify_using<zero_mem>(
            (void*)((size_t)p + OS_PAGE_SIZE),
            bits::align_up(size, OS_PAGE_SIZE) - OS_PAGE_SIZE);
        }
        else
        {
          // This is a superslab that has not been decommitted.
          if (zero_mem == YesZero)
            memory_provider.template zero<true>(
              p, bits::align_up(size, OS_PAGE_SIZE));
        }
      }

      return p;
    }

    void dealloc(void* p, size_t large_class)
    {
      memory_provider.large_stack[large_class].push((Largeslab*)p);
    }
  };

  using GlobalVirtual = MemoryProviderStateMixin<Pal>;
  /**
   * The memory provider that will be used if no other provider is explicitly
   * passed as an argument.
   */
  HEADER_GLOBAL GlobalVirtual default_memory_provider;
}