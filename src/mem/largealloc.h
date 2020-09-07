#pragma once

#include "../ds/flaglock.h"
#include "../ds/helpers.h"
#include "../ds/mpmcstack.h"
#include "../pal/pal.h"
#include "address_space.h"
#include "allocstats.h"
#include "baseslab.h"
#include "sizeclass.h"

#include <new>
#include <string.h>

namespace snmalloc
{
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL>
  class MemoryProviderStateMixin;

  class Largeslab : public Baseslab
  {
    // This is the view of a contiguous memory area when it is being kept
    // in the global size-classed caches of available contiguous memory areas.
  private:
    template<class a, Construction c>
    friend class MPMCStack;
    template<SNMALLOC_CONCEPT(ConceptPAL) PAL>
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
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL>
  class MemoryProviderStateMixin : public PalNotificationObject
  {
    /**
     * Simple flag for checking if another instance of lazy-decommit is
     * running
     */
    std::atomic_flag lazy_decommit_guard = {};

    /**
     * Manages address space for this memory provider.
     */
    AddressSpaceManager<PAL> address_space = {};

    /**
     * High-water mark of used memory.
     */
    std::atomic<size_t> peak_memory_used_bytes{0};

  public:
    using Pal = PAL;

    /**
     * Memory current available in large_stacks
     */
    std::atomic<size_t> available_large_chunks_in_bytes{0};

    /**
     * Stack of large allocations that have been returned for reuse.
     */
    ModArray<NUM_LARGE_CLASSES, MPMCStack<Largeslab, RequiresInit>> large_stack;

    /**
     * Make a new memory provide for this PAL.
     */
    static MemoryProviderStateMixin<PAL>* make() noexcept
    {
      // Temporary stack-based storage to start the allocator in.
      MemoryProviderStateMixin<PAL> local{};

      // Allocate permanent storage for the allocator usung temporary allocator
      MemoryProviderStateMixin<PAL>* allocated =
        local.alloc_chunk<MemoryProviderStateMixin<PAL>, 1>();

      if (allocated == nullptr)
        error("Failed to initialise system!");

#ifdef GCC_VERSION_EIGHT_PLUS
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wclass-memaccess"
#endif
      // Put temporary allocator we have used, into the permanent storage.
      // memcpy is safe as this is entirely single threaded: the move
      // constructors were removed as unsafe to move std::atomic in a
      // concurrent setting.
      ::memcpy(
        &(allocated->address_space),
        &(local.address_space),
        sizeof(AddressSpaceManager<PAL>));
#ifdef GCC_VERSION_EIGHT_PLUS
#  pragma GCC diagnostic pop
#endif

      // Register this allocator for low-memory call-backs
      if constexpr (pal_supports<LowMemoryNotification, PAL>)
      {
        allocated->PalNotificationObject::pal_notify = &(allocated->process);
        PAL::register_for_low_memory_callback(allocated);
      }

      return allocated;
    }

  private:
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

    /***
     * Method for callback object to perform lazy decommit.
     */
    static void process(PalNotificationObject* p)
    {
      // Unsafe downcast here. Don't want vtable and RTTI.
      auto self = reinterpret_cast<MemoryProviderStateMixin<PAL>*>(p);
      self->lazy_decommit();
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
      size = bits::next_pow2(bits::max(size, alignment));
      void* p = address_space.template reserve<true>(size);
      if (p == nullptr)
        return nullptr;

      peak_memory_used_bytes += size;

      return new (p) T(std::forward<Args...>(args)...);
    }

    template<bool committed>
    void* reserve(size_t large_class) noexcept
    {
      size_t size = bits::one_at_bit(SUPERSLAB_BITS) << large_class;
      peak_memory_used_bytes += size;
      return address_space.template reserve<committed>(size);
    }

    /**
     * Returns a pair of current memory usage and peak memory usage.
     * Both statistics are very coarse-grained.
     */
    std::pair<size_t, size_t> memory_usage()
    {
      size_t avail = available_large_chunks_in_bytes;
      size_t peak = peak_memory_used_bytes;
      return {peak - avail, peak};
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

      if (p == nullptr)
      {
        p = memory_provider.template reserve<false>(large_class);
        if (p == nullptr)
          return nullptr;
        MemoryProvider::Pal::template notify_using<zero_mem>(p, rsize);
      }
      else
      {
        stats.superslab_pop();
        memory_provider.available_large_chunks_in_bytes -= rsize;

        // Cross-reference alloc.h's large_dealloc decommitment condition.
        bool decommitted =
          ((decommit_strategy == DecommitSuperLazy) &&
           (static_cast<Baseslab*>(p)->get_kind() == Decommitted)) ||
          (large_class > 0) || (decommit_strategy == DecommitSuper);

        if (decommitted)
        {
          // The first page is already in "use" for the stack element,
          // this will need zeroing for a YesZero call.
          if constexpr (zero_mem == YesZero)
            MemoryProvider::Pal::template zero<true>(p, OS_PAGE_SIZE);

          // Notify we are using the rest of the allocation.
          // Passing zero_mem ensures the PAL provides zeroed pages if
          // required.
          MemoryProvider::Pal::template notify_using<zero_mem>(
            pointer_offset(p, OS_PAGE_SIZE), rsize - OS_PAGE_SIZE);
        }
        else
        {
          // This is a superslab that has not been decommitted.
          if constexpr (zero_mem == YesZero)
            MemoryProvider::Pal::template zero<true>(
              p, bits::align_up(size, OS_PAGE_SIZE));
          else
            UNUSED(size);
        }
      }

      SNMALLOC_ASSERT(p == pointer_align_up(p, rsize));
      return p;
    }

    void dealloc(void* p, size_t large_class)
    {
      if constexpr (decommit_strategy == DecommitSuperLazy)
      {
        static_assert(
          pal_supports<LowMemoryNotification, typename MemoryProvider::Pal>,
          "A lazy decommit strategy cannot be implemented on platforms "
          "without low memory notifications");
      }

      size_t rsize = bits::one_at_bit(SUPERSLAB_BITS) << large_class;

      // Cross-reference largealloc's alloc() decommitted condition.
      if (
        (decommit_strategy != DecommitNone) &&
        (large_class != 0 || decommit_strategy == DecommitSuper))
      {
        MemoryProvider::Pal::notify_not_using(
          pointer_offset(p, OS_PAGE_SIZE), rsize - OS_PAGE_SIZE);
      }

      stats.superslab_push();
      memory_provider.available_large_chunks_in_bytes += rsize;
      memory_provider.large_stack[large_class].push(static_cast<Largeslab*>(p));
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
