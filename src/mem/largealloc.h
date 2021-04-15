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
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL, typename ArenaMap>
  class MemoryProviderStateMixin;

  class Largeslab : public Baseslab
  {
    // This is the view of a contiguous memory area when it is being kept
    // in the global size-classed caches of available contiguous memory areas.
  private:
    template<
      class a,
      Construction c,
      template<typename>
      typename P,
      template<typename>
      typename AP>
    friend class MPMCStack;
    template<SNMALLOC_CONCEPT(ConceptPAL) PAL, typename ArenaMap>
    friend class MemoryProviderStateMixin;
    AtomicCapPtr<Largeslab, CBChunk> next = nullptr;

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
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL, typename ArenaMap>
  class MemoryProviderStateMixin
  {
    /**
     * Simple flag for checking if another instance of lazy-decommit is
     * running
     */
    std::atomic_flag lazy_decommit_guard = {};

    /**
     * Instantiate the ArenaMap here.
     *
     * In most cases, this will be a purely static object (a DefaultArenaMap
     * using a GlobalPagemapTemplate or ExternalGlobalPagemapTemplate).  For
     * sandboxes, this may have per-instance state (e.g., the sandbox root);
     * presently, that's handled by the MemoryProviderStateMixin constructor
     * that takes a pointer to address space it owns.  There is some
     * non-orthogonality of concerns here.
     */
    ArenaMap arena_map = {};

    using ASM = AddressSpaceManager<PAL, ArenaMap>;
    /**
     * Manages address space for this memory provider.
     */
    ASM address_space = {};

    /**
     * High-water mark of used memory.
     */
    std::atomic<size_t> peak_memory_used_bytes{0};

    /**
     * Memory current available in large_stacks
     */
    std::atomic<size_t> available_large_chunks_in_bytes{0};

    /**
     * Stack of large allocations that have been returned for reuse.
     */
    ModArray<
      NUM_LARGE_CLASSES,
      MPMCStack<Largeslab, RequiresInit, CapPtrCBChunk, AtomicCapPtrCBChunk>>
      large_stack;

  public:
    using Pal = PAL;

    /**
     * Pop an allocation from a large-allocation stack.  This is safe to call
     * concurrently with other acceses.  If there is no large allocation on a
     * particular stack then this will return `nullptr`.
     */
    SNMALLOC_FAST_PATH CapPtr<Largeslab, CBChunk>
    pop_large_stack(size_t large_class)
    {
      auto p = large_stack[large_class].pop();
      if (p != nullptr)
      {
        const size_t rsize = bits::one_at_bit(SUPERSLAB_BITS) << large_class;
        available_large_chunks_in_bytes -= rsize;
      }
      return p;
    }

    /**
     * Push `slab` onto the large-allocation stack associated with the size
     * class specified by `large_class`.  Always succeeds.
     */
    SNMALLOC_FAST_PATH void
    push_large_stack(CapPtr<Largeslab, CBChunk> slab, size_t large_class)
    {
      const size_t rsize = bits::one_at_bit(SUPERSLAB_BITS) << large_class;
      available_large_chunks_in_bytes += rsize;
      large_stack[large_class].push(slab);
    }

    /**
     * Default constructor.  This constructs a memory provider that doesn't yet
     * own any memory, but which can claim memory from the PAL.
     */
    MemoryProviderStateMixin() = default;

    /**
     * Construct a memory provider owning some memory.  The PAL provided with
     * memory providers constructed in this way does not have to be able to
     * allocate memory, if the initial reservation is sufficient.
     */
    MemoryProviderStateMixin(CapPtr<void, CBChunk> start, size_t len)
    : address_space(start, len)
    {}
    /**
     * Make a new memory provide for this PAL.
     */
    static MemoryProviderStateMixin* make() noexcept
    {
      // Temporary stack-based storage to start the allocator in.
      ASM local_asm{};
      ArenaMap local_am{};

      // Allocate permanent storage for the allocator usung temporary allocator
      MemoryProviderStateMixin* allocated =
        local_asm
          .template reserve_with_left_over<true>(
            sizeof(MemoryProviderStateMixin), local_am)
          .template as_static<MemoryProviderStateMixin>()
          .unsafe_capptr;

      if (allocated == nullptr)
        error("Failed to initialise system!");

      // Move address range inside itself
      allocated->address_space = std::move(local_asm);
      allocated->arena_map = std::move(local_am);

      // Register this allocator for low-memory call-backs
      if constexpr (pal_supports<LowMemoryNotification, PAL>)
      {
        auto callback =
          allocated->template alloc_chunk<LowMemoryNotificationObject, 1>(
            allocated);
        PAL::register_for_low_memory_callback(callback);
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
        CapPtr<Largeslab, CBChunk> slab = large_stack[large_class].pop_all();
        while (slab != nullptr)
        {
          // Decommit all except for the first page and then put it back on
          // the stack.
          if (slab->get_kind() != Decommitted)
          {
            PAL::notify_not_using(
              pointer_offset(slab.unsafe_capptr, OS_PAGE_SIZE), decommit_size);
          }
          // Once we've removed these from the stack, there will be no
          // concurrent accesses and removal should have established a
          // happens-before relationship, so it's safe to use relaxed loads
          // here.
          auto next = slab->next.load(std::memory_order_relaxed);
          large_stack[large_class].push(CapPtr<Largeslab, CBChunk>(
            new (slab.unsafe_capptr) Decommittedslab()));
          slab = next;
        }
      }
      lazy_decommit_guard.clear();
    }

    class LowMemoryNotificationObject : public PalNotificationObject
    {
      MemoryProviderStateMixin* memory_provider;

      /***
       * Method for callback object to perform lazy decommit.
       */
      static void process(PalNotificationObject* p)
      {
        // Unsafe downcast here. Don't want vtable and RTTI.
        auto self = reinterpret_cast<LowMemoryNotificationObject*>(p);
        self->memory_provider->lazy_decommit();
      }

    public:
      LowMemoryNotificationObject(MemoryProviderStateMixin* memory_provider)
      : PalNotificationObject(&process), memory_provider(memory_provider)
      {}
    };

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
      size = bits::max(size, alignment);
      auto p =
        address_space.template reserve_with_left_over<true>(size, arena_map);
      if (p == nullptr)
        return nullptr;

      peak_memory_used_bytes += size;

      return new (p.unsafe_capptr) T(std::forward<Args...>(args)...);
    }

    template<bool committed>
    CapPtr<Largeslab, CBChunk> reserve(size_t large_class) noexcept
    {
      size_t size = bits::one_at_bit(SUPERSLAB_BITS) << large_class;
      peak_memory_used_bytes += size;
      return address_space.template reserve<committed>(size, arena_map)
        .template as_static<Largeslab>();
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

    template<typename T, typename U, SNMALLOC_CONCEPT(capptr_bounds::c) B>
    SNMALLOC_FAST_PATH CapPtr<T, CBArena> capptr_amplify(CapPtr<U, B> r)
    {
      return arena_map.template capptr_amplify<T, U, B>(r);
    }

    template<typename T>
    SNMALLOC_FAST_PATH CapPtr<T, CBAllocE> capptr_dewild(CapPtr<T, CBAllocEW> p)
    {
      return Aal::capptr_dewild(p);
    }

    ArenaMap& arenamap()
    {
      return arena_map;
    }
  };

  using Stats = AllocStats<NUM_SIZECLASSES, NUM_LARGE_CLASSES>;

  template<class MemoryProvider>
  class LargeAlloc
  {
  public:
    // This will be a zero-size structure if stats are not enabled.
    Stats stats;

    MemoryProvider& memory_provider;

    LargeAlloc(MemoryProvider& mp) : memory_provider(mp) {}

    template<ZeroMem zero_mem = NoZero>
    CapPtr<Largeslab, CBChunk>
    alloc(size_t large_class, size_t rsize, size_t size)
    {
      SNMALLOC_ASSERT(
        (bits::one_at_bit(SUPERSLAB_BITS) << large_class) == rsize);

      CapPtr<Largeslab, CBChunk> p =
        memory_provider.pop_large_stack(large_class);

      if (p == nullptr)
      {
        p = memory_provider.template reserve<false>(large_class);
        if (p == nullptr)
          return nullptr;
        MemoryProvider::Pal::template notify_using<zero_mem>(
          p.unsafe_capptr, rsize);
      }
      else
      {
        stats.superslab_pop();

        // Cross-reference alloc.h's large_dealloc decommitment condition.
        bool decommitted =
          ((decommit_strategy == DecommitSuperLazy) &&
           (p.template as_static<Baseslab>().unsafe_capptr->get_kind() ==
            Decommitted)) ||
          (large_class > 0) || (decommit_strategy == DecommitSuper);

        if (decommitted)
        {
          // The first page is already in "use" for the stack element,
          // this will need zeroing for a YesZero call.
          if constexpr (zero_mem == YesZero)
            pal_zero<typename MemoryProvider::Pal, true>(p, OS_PAGE_SIZE);

          // Notify we are using the rest of the allocation.
          // Passing zero_mem ensures the PAL provides zeroed pages if
          // required.
          MemoryProvider::Pal::template notify_using<zero_mem>(
            pointer_offset(p.unsafe_capptr, OS_PAGE_SIZE),
            rsize - OS_PAGE_SIZE);
        }
        else
        {
          // This is a superslab that has not been decommitted.
          if constexpr (zero_mem == YesZero)
            pal_zero<typename MemoryProvider::Pal, true>(
              p, bits::align_up(size, OS_PAGE_SIZE));
          else
            UNUSED(size);
        }
      }

      SNMALLOC_ASSERT(p.as_void() == pointer_align_up(p.as_void(), rsize));
      return p;
    }

    void dealloc(CapPtr<Largeslab, CBChunk> p, size_t large_class)
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
          pointer_offset(p, OS_PAGE_SIZE).unsafe_capptr, rsize - OS_PAGE_SIZE);
      }

      stats.superslab_push();
      memory_provider.push_large_stack(p, large_class);
    }

    template<
      typename T = void,
      typename U,
      SNMALLOC_CONCEPT(capptr_bounds::c) B>
    SNMALLOC_FAST_PATH CapPtr<T, CBArena> capptr_amplify(CapPtr<U, B> r)
    {
      return memory_provider.template capptr_amplify<T, U, B>(r);
    }

    template<typename T>
    SNMALLOC_FAST_PATH CapPtr<T, CBAllocE> capptr_dewild(CapPtr<T, CBAllocEW> p)
    {
      return memory_provider.capptr_dewild(p);
    }
  };

  struct DefaultPrimAlloc;

#ifndef SNMALLOC_DEFAULT_MEMORY_PROVIDER
#  define SNMALLOC_DEFAULT_MEMORY_PROVIDER \
    MemoryProviderStateMixin<Pal, DefaultArenaMap<Pal, DefaultPrimAlloc>>
#endif

  /**
   * The type of the default memory allocator.  This can be changed by defining
   * `SNMALLOC_DEFAULT_MEMORY_PROVIDER` before including this file.  By default
   * it is `MemoryProviderStateMixin<Pal>` a class that allocates directly from
   * the platform abstraction layer.
   */
  using GlobalVirtual = SNMALLOC_DEFAULT_MEMORY_PROVIDER;

  /**
   * The memory provider that will be used if no other provider is explicitly
   * passed as an argument.
   */
  inline GlobalVirtual& default_memory_provider()
  {
    return *(Singleton<GlobalVirtual*, GlobalVirtual::make>::get());
  }

  struct DefaultPrimAlloc
  {
    template<typename T, size_t alignment, typename... Args>
    static T* alloc_chunk(Args&&... args)
    {
      return default_memory_provider().alloc_chunk<T, alignment>(args...);
    }
  };
} // namespace snmalloc
