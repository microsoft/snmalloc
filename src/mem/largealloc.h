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
  template<class PAL>
  class MemoryProviderStateMixin;

  class Largeslab : public Baseslab
  {
    // This is the view of a contiguous memory area when it is being kept
    // in the global size-classed caches of available contiguous memory areas.
  private:
    template<class a, Construction c>
    friend class MPMCStack;
    template<class PAL>
    friend class MemoryProviderStateMixin;
    std::atomic<Largeslab*> next;

  public:
    void init()
    {
      kind = Large;
    }
  };

  struct Decommittedslab : public Largeslab
  {
    Decommittedslab()
    {
      kind = Decommitted;
    }
  };

  // This represents the state that the large allcoator needs to add to the
  // global state of the allocator.  This is currently stored in the memory
  // provider, so we add this in.
  template<class PAL>
  class MemoryProviderStateMixin : public PAL
  {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    size_t bump;
    size_t remaining;

    std::pair<void*, size_t> reserve_block() noexcept
    {
      size_t size = SUPERSLAB_SIZE;
      void* r = ((PAL*)this)->template reserve<false>(&size, SUPERSLAB_SIZE);

      if (size < SUPERSLAB_SIZE)
        error("out of memory");

      ((PAL*)this)->template notify_using<NoZero>(r, OS_PAGE_SIZE);
      return std::make_pair(r, size);
    }

    void new_block()
    {
      auto r_size = reserve_block();
      bump = (size_t)r_size.first;
      remaining = r_size.second;
    }

    /**
     * The last time we saw a low memory notification.
     */
    std::atomic<uint64_t> last_low_memory_epoch = 0;
    std::atomic_flag lazy_decommit_guard;
    void lazy_decommit()
    {
      // If another thread is try to do lazy decommit, let it continue.  If
      // we try to parallelise this, we'll most likely end up waiting on the
      // same page table locks.
      if (!lazy_decommit_guard.test_and_set())
      {
        return;
      }
      // When we hit low memory, iterate over size classes and decommit all of
      // the memory that we can.  Start with the small size classes so that we
      // hit cached superslabs first.
      // FIXME: We probably shouldn't do this all at once.
      for (size_t large_class = 0; large_class < NUM_LARGE_CLASSES;
           large_class++)
      {
        size_t rsize = ((size_t)1 << SUPERSLAB_BITS) << large_class;
        size_t decommit_size = rsize - OS_PAGE_SIZE;
        // Grab all of the chunks of this size class.
        auto* slab = large_stack[large_class].pop_all();
        while (slab)
        {
          // Decommit all except for the first page and then put it back on
          // the stack.
          if (slab->get_kind() != Decommitted)
          {
            PAL::notify_not_using(((char*)slab) + OS_PAGE_SIZE, decommit_size);
          }
          // Once we've removed these from the stack, there will be no
          // concurrent accesses and removal should have established a
          // happens-before relationship, so it's safe to use relaxed loads
          // here.
          auto next = slab->next.load(std::memory_order_relaxed);
          large_stack[large_class].push(new (slab) Decommittedslab());
          slab = next;
        }
      }

      lazy_decommit_guard.clear();
    }

  public:
    template<PalFeatures F, typename P = PAL>
    constexpr static bool pal_supports()
    {
      return (P::pal_features & F) == F;
    }

  private:
    /**
     * Wrapper that is instantiated only if the memory provider supports low
     * memory notifications and forwards the call to the memory provider.
     */
    template<typename M>
    ALWAYSINLINE uint64_t low_mem_epoch(
      std::enable_if_t<pal_supports<LowMemoryNotification, M>(), int> = 0)
    {
      return PAL::low_memory_epoch();
    }

    /**
     * Default implementations that is instantiated when the memory provider
     * does not support low memory notifications and always returns 0 for the
     * epoch.
     */
    template<typename M>
    ALWAYSINLINE uint64_t low_memory_epoch(
      std::enable_if_t<!pal_supports<LowMemoryNotification, M>(), int> = 0)
    {
      return 0;
    }

  public:
    /**
     * Stack of large allocations that have been returned for reuse.
     */
    ModArray<NUM_LARGE_CLASSES, MPMCStack<Largeslab, PreZeroed>> large_stack;

    /**
     * Primitive allocator for structure that are required before
     * the allocator can be running.
     */
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

      ((PAL*)this)
        ->template notify_using<NoZero>(
          (void*)page_start, page_end - page_start);

      return p;
    }

    /**
     * Returns the number of low memory notifications that have been received
     * (over the lifetime of this process).  If the underlying system does not
     * support low memory notifications, this will return 0.
     */
    ALWAYSINLINE
    uint64_t low_memory_epoch()
    {
      return low_mem_epoch<PAL>();
    }

    ALWAYSINLINE void lazy_decommit_if_needed()
    {
#ifdef TEST_LAZY_DECOMMIT
      static_assert(
        TEST_LAZY_DECOMMIT > 0,
        "TEST_LAZY_DECOMMIT must be a positive integer value.");
      static std::atomic<uint64_t> counter;
      auto c = counter++;
      if (c % TEST_LAZY_DECOMMIT == 0)
      {
        lazy_decommit();
      }
#else
      if constexpr (decommit_strategy == DecommitSuperLazy)
      {
        auto new_epoch = low_memory_epoch();
        auto old_epoch = last_low_memory_epoch.load(std::memory_order_acquire);
        if (new_epoch > old_epoch)
        {
          // Try to update the epoch to the value that we've seen.  If
          // another thread has seen a newer epoch than us (or done the same
          // update) let them win.
          do
          {
            last_low_memory_epoch.compare_exchange_strong(old_epoch, new_epoch);
          } while (old_epoch <= new_epoch);
          lazy_decommit();
        }
      }
#endif
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
        if constexpr (allow_reserve == YesReserve)
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
      memory_provider.lazy_decommit_if_needed();

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
        if constexpr (decommit_strategy == DecommitSuperLazy)
        {
          if (static_cast<Baseslab*>(p)->get_kind() == Decommitted)
          {
            // The first page is already in "use" for the stack element,
            // this will need zeroing for a YesZero call.
            if constexpr (zero_mem == YesZero)
              memory_provider.template zero<true>(p, OS_PAGE_SIZE);

            // Notify we are using the rest of the allocation.
            // Passing zero_mem ensures the PAL provides zeroed pages if
            // required.
            memory_provider.template notify_using<zero_mem>(
              (void*)((size_t)p + OS_PAGE_SIZE),
              bits::align_up(size, OS_PAGE_SIZE) - OS_PAGE_SIZE);
          }
          else
          {
            if constexpr (zero_mem == YesZero)
              memory_provider.template zero<true>(
                p, bits::align_up(size, OS_PAGE_SIZE));
          }
        }
        if ((decommit_strategy != DecommitNone) || (large_class > 0))
        {
          // The first page is already in "use" for the stack element,
          // this will need zeroing for a YesZero call.
          if constexpr (zero_mem == YesZero)
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
          if constexpr (zero_mem == YesZero)
            memory_provider.template zero<true>(
              p, bits::align_up(size, OS_PAGE_SIZE));
        }
      }

      return p;
    }

    void dealloc(void* p, size_t large_class)
    {
      memory_provider.large_stack[large_class].push(static_cast<Largeslab*>(p));
      memory_provider.lazy_decommit_if_needed();
    }
  };

  using GlobalVirtual = MemoryProviderStateMixin<Pal>;
  /**
   * The memory provider that will be used if no other provider is explicitly
   * passed as an argument.
   */
  HEADER_GLOBAL GlobalVirtual default_memory_provider;
}
