#pragma once

#include "../ds/flaglock.h"
#include "../ds/helpers.h"
#include "../ds/mpmcstack.h"
#include "../pal/pal.h"
#include "allocstats.h"
#include "baseslab.h"
#include "sizeclass.h"

#include <string.h>
#include <new>

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

  /**
   * A slab that has been decommitted.  The first page remains committed and
   * the only fields that are guaranteed to exist are the kind and next
   * pointer from the superclass.
   */
  struct Decommittedslab : public Largeslab
  {
    /**
     * Constructor.  Expected to be called via placement new into some memory
     * that was formerly a superslab or large allocation and is now just some
     * spare address space.
     */
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
    /**
     * Flag to protect the bump allocator
     **/
    std::atomic_flag lock = ATOMIC_FLAG_INIT;

    /**
     * Pointer to block being bump allocated
     **/
    void* bump = nullptr;

    /**
     * Space remaining in this block being bump allocated
     **/
    size_t remaining = 0;

    /**
     * The last time we saw a low memory notification.
     */
    std::atomic<uint64_t> last_low_memory_epoch = 0;

    /**
     * Simple flag for checking if another instance of lazy-decommit is
     * running
     **/
    std::atomic_flag lazy_decommit_guard = {};

  public:
    /**
     * Stack of large allocations that have been returned for reuse.
     */
    ModArray<NUM_LARGE_CLASSES, MPMCStack<Largeslab, RequiresInit>> large_stack;

    /**
     * Make a new memory provide for this PAL.
     **/
    static MemoryProviderStateMixin<PAL>* make() noexcept
    {
      // Temporary stack-based storage to start the allocator in.
      MemoryProviderStateMixin<PAL> local;

      // Allocate permanent storage for the allocator usung temporary allocator
      MemoryProviderStateMixin<PAL>* allocated =
        local.alloc_chunk<MemoryProviderStateMixin<PAL>, 1>();

      // Put temporary allocator we have used, into the permanent storage.
      // memcpy is safe as this is entirely single threaded.
      memcpy(allocated, &local, sizeof(MemoryProviderStateMixin<PAL>));

      return allocated;
    }

  private:
    void new_block()
    {
      // Reserve the smallest large_class which is SUPERSLAB_SIZE
      void* r = reserve<false>(0);
      PAL::template notify_using<NoZero>(r, OS_PAGE_SIZE);

      bump = r;
      remaining = SUPERSLAB_SIZE;
    }

    SNMALLOC_SLOW_PATH void lazy_decommit()
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
      // FIXME: We currently Decommit all the sizeclasses larger than 0.
      for (size_t large_class = 0; large_class < NUM_LARGE_CLASSES;
           large_class++)
      {
        if (!PAL::expensive_low_memory_check())
        {
          break;
        }
        size_t rsize = bits::one_at_bit(SUPERSLAB_BITS) << large_class;
        size_t decommit_size = rsize - OS_PAGE_SIZE;
        // Grab all of the chunks of this size class.
        auto* slab = large_stack[large_class].pop_all();
        while (slab)
        {
          // Decommit all except for the first page and then put it back on
          // the stack.
          if (slab->get_kind() != Decommitted)
          {
            PAL::notify_not_using(
              pointer_offset(slab, OS_PAGE_SIZE), decommit_size);
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

    void push_space(address_t start, size_t large_class)
    {
      void* p = pointer_cast<void>(start);
      if (large_class > 0)
        PAL::template notify_using<YesZero>(p, OS_PAGE_SIZE);
      else
      {
        if (decommit_strategy == DecommitSuperLazy)
        {
          PAL::template notify_using<YesZero>(p, OS_PAGE_SIZE);
          p = new (p) Decommittedslab();
        }
        else
          PAL::template notify_using<YesZero>(p, SUPERSLAB_SIZE);
      }
      large_stack[large_class].push(reinterpret_cast<Largeslab*>(p));
    }

  public:
    /**
     * Primitive allocator for structure that are required before
     * the allocator can be running.
     */
    template<typename T, size_t alignment, typename... Args>
    T* alloc_chunk(Args&&... args)
    {
      // Cache line align
      size_t size = bits::align_up(sizeof(T), 64);

      void* p;
      {
        FlagLock f(lock);

        if constexpr (alignment != 0)
        {
          char* aligned_bump = pointer_align_up<alignment, char>(bump);

          size_t bump_delta = pointer_diff(bump, aligned_bump);

          if (bump_delta > remaining)
          {
            new_block();
          }
          else
          {
            remaining -= bump_delta;
            bump = aligned_bump;
          }
        }

        if (remaining < size)
        {
          new_block();
        }

        p = bump;
        bump = pointer_offset(bump, size);
        remaining -= size;
      }

      auto page_start = pointer_align_down<OS_PAGE_SIZE, char>(p);
      auto page_end =
        pointer_align_up<OS_PAGE_SIZE, char>(pointer_offset(p, size));

      PAL::template notify_using<NoZero>(
        page_start, static_cast<size_t>(page_end - page_start));

      return new (p) T(std::forward<Args...>(args)...);
    }

    /**
     * Returns the number of low memory notifications that have been received
     * (over the lifetime of this process).  If the underlying system does not
     * support low memory notifications, this will return 0.
     */
    SNMALLOC_FAST_PATH
    uint64_t low_memory_epoch()
    {
      if constexpr (pal_supports<LowMemoryNotification, PAL>)
      {
        return PAL::low_memory_epoch();
      }
      else
      {
        return 0;
      }
    }

    template<bool committed>
    void* reserve(size_t large_class) noexcept
    {
      size_t size = bits::one_at_bit(SUPERSLAB_BITS) << large_class;
      size_t align = size;

      if constexpr (pal_supports<AlignedAllocation, PAL>)
      {
        return PAL::template reserve<committed>(size, align);
      }
      else
      {
        // Reserve 4 times the amount, and put aligned leftovers into the
        // large_stack
        size_t request = bits::max(size * 4, SUPERSLAB_SIZE * 8);
        void* p = PAL::template reserve<false>(request);

        address_t p0 = address_cast(p);
        address_t start = bits::align_up(p0, align);
        address_t p1 = p0 + request;
        address_t end = start + size;

        for (; end < bits::align_down(p1, align); end += size)
        {
          push_space(end, large_class);
        }

        // Put offcuts before alignment into the large stack
        address_t offcut_end = start;
        address_t offcut_start;
        for (size_t i = large_class; i > 0;)
        {
          i--;
          size_t offcut_align = bits::one_at_bit(SUPERSLAB_BITS) << i;
          offcut_start = bits::align_up(p0, offcut_align);
          if (offcut_start != offcut_end)
          {
            push_space(offcut_start, i);
            offcut_end = offcut_start;
          }
        }

        // Put offcuts after returned block into the large stack
        offcut_start = end;
        for (size_t i = large_class; i > 0;)
        {
          i--;
          auto offcut_align = bits::one_at_bit(SUPERSLAB_BITS) << i;
          offcut_end = bits::align_down(p1, offcut_align);
          if (offcut_start != offcut_end)
          {
            push_space(offcut_start, i);
            offcut_start = offcut_end;
          }
        }

        void* result = pointer_cast<void>(start);
        if (committed)
          PAL::template notify_using<NoZero>(result, size);

        return result;
      }
    }

    SNMALLOC_FAST_PATH void lazy_decommit_if_needed()
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
            if (last_low_memory_epoch.compare_exchange_strong(
                  old_epoch, new_epoch))
            {
              lazy_decommit();
            }
          } while (old_epoch < new_epoch);
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
  public:
    // This will be a zero-size structure if stats are not enabled.
    Stats stats;

    MemoryProvider& memory_provider;

    LargeAlloc(MemoryProvider& mp) : memory_provider(mp) {}

    template<ZeroMem zero_mem = NoZero, AllowReserve allow_reserve = YesReserve>
    void* alloc(size_t large_class, size_t size)
    {
      size_t rsize = bits::one_at_bit(SUPERSLAB_BITS) << large_class;
      // For superslab size, we always commit the whole range.
      if (large_class == 0)
        size = rsize;

      void* p = memory_provider.large_stack[large_class].pop();
      memory_provider.lazy_decommit_if_needed();

      if (p == nullptr)
      {
        p = memory_provider.template reserve<false>(large_class);
        memory_provider.template notify_using<zero_mem>(p, size);
      }
      else
      {
        stats.superslab_pop();

        // Cross-reference alloc.h's large_dealloc decommitment condition
        // and lazy_decommit_if_needed.
        bool decommitted =
          ((decommit_strategy == DecommitSuperLazy) &&
           (static_cast<Baseslab*>(p)->get_kind() == Decommitted)) ||
          (large_class > 0) || (decommit_strategy == DecommitSuper);

        if (decommitted)
        {
          // The first page is already in "use" for the stack element,
          // this will need zeroing for a YesZero call.
          if constexpr (zero_mem == YesZero)
            memory_provider.template zero<true>(p, OS_PAGE_SIZE);

          // Notify we are using the rest of the allocation.
          // Passing zero_mem ensures the PAL provides zeroed pages if
          // required.
          memory_provider.template notify_using<zero_mem>(
            pointer_offset(p, OS_PAGE_SIZE),
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

      assert(p == pointer_align_up(p, rsize));
      return p;
    }

    void dealloc(void* p, size_t large_class)
    {
      if constexpr (decommit_strategy == DecommitSuperLazy)
      {
        static_assert(
          pal_supports<LowMemoryNotification, MemoryProvider>,
          "A lazy decommit strategy cannot be implemented on platforms "
          "without low memory notifications");
      }

      // Cross-reference largealloc's alloc() decommitted condition.
      if (
        (decommit_strategy != DecommitNone) &&
        (large_class != 0 || decommit_strategy == DecommitSuper))
      {
        size_t rsize = bits::one_at_bit(SUPERSLAB_BITS) << large_class;

        memory_provider.notify_not_using(
          pointer_offset(p, OS_PAGE_SIZE), rsize - OS_PAGE_SIZE);
      }

      stats.superslab_push();
      memory_provider.large_stack[large_class].push(static_cast<Largeslab*>(p));
      memory_provider.lazy_decommit_if_needed();
    }
  };

  using GlobalVirtual = MemoryProviderStateMixin<Pal>;
  /**
   * The memory provider that will be used if no other provider is explicitly
   * passed as an argument.
   */
  inline GlobalVirtual& default_memory_provider()
  {
    return *(Singleton<GlobalVirtual*, GlobalVirtual::make>::get());
  }
} // namespace snmalloc
