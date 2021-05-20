#pragma once

#include "../ds/flaglock.h"
#include "../ds/helpers.h"
#include "../ds/mpmcstack.h"
#include "../pal/pal.h"
#include "address_space.h"
#include "allocstats.h"
#include "baseslab.h"
#include "fastcache.h"
#include "metadataalloc.h"
#include "sizeclass.h"

#include <new>
#include <string.h>

namespace snmalloc
{
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
    friend class ChunkAllocator;
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
  class ChunkAllocator : public MetadataAllocator<PAL, ArenaMap>
  {
  private:
    /**
     * Simple flag for checking if another instance of lazy-decommit is
     * running
     */
    std::atomic_flag lazy_decommit_guard = {};

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
    using base = MetadataAllocator<PAL, ArenaMap>;

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
    ChunkAllocator() = default;

    /**
     * Construct a memory provider owning some memory.  The PAL provided with
     * memory providers constructed in this way does not have to be able to
     * allocate memory, if the initial reservation is sufficient.
     */
    ChunkAllocator(CapPtr<void, CBChunk> start, size_t len) : base(start, len)
    {}

    /**
     * Construct a memory provider using an already initialised metadata
     * allocator used during bootstrapping
     */
    ChunkAllocator(base& b) : base(b) {}

    /**
     * Make a new memory provide for this PAL.
     */
    static ChunkAllocator* make() noexcept
    {
      // Temporary stack-based storage to start the allocator in.
      base local_alloc{};

      // Allocate permanent storage for the allocator using temporary allocator
      ChunkAllocator* allocated =
        local_alloc.template alloc_meta<ChunkAllocator, 1>(local_alloc);

      if (allocated == nullptr)
        error("Failed to initialise system!");

      // Register this allocator for low-memory call-backs
      if constexpr (pal_supports<LowMemoryNotification, PAL>)
      {
        auto callback =
          allocated->template alloc_meta<LowMemoryNotificationObject, 1>(
            allocated);
        PAL::register_for_low_memory_callback(callback);
      }

      // TODO: This should initialise ChunkMap at this point.
      // User allocation need the chunk map to exist.

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
      ChunkAllocator* memory_provider;

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
      LowMemoryNotificationObject(ChunkAllocator* memory_provider)
      : PalNotificationObject(&process), memory_provider(memory_provider)
      {}
    };

  public:
    template<bool committed>
    CapPtr<Largeslab, CBChunk> reserve(size_t large_class) noexcept
    {
      size_t size = bits::one_at_bit(SUPERSLAB_BITS) << large_class;
      peak_memory_used_bytes += size;
      return base::address_space.template reserve<committed>(size)
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

    template<typename T, typename U, capptr_bounds B>
    SNMALLOC_FAST_PATH CapPtr<T, CBArena> capptr_amplify(CapPtr<U, B> r)
    {
      return arenamap().template capptr_amplify<T, U, B>(r);
    }

    ArenaMap& arenamap()
    {
      return base::get_address_space().arenamap();
    }
  };

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

    template<typename T = void, typename U, capptr_bounds B>
    SNMALLOC_FAST_PATH CapPtr<T, CBArena> capptr_amplify(CapPtr<U, B> r)
    {
      return memory_provider.template capptr_amplify<T, U, B>(r);
    }
  };

  struct DefaultPrimAlloc;

#ifndef SNMALLOC_DEFAULT_CHUNK_ALLOCATOR
#  define SNMALLOC_DEFAULT_CHUNK_ALLOCATOR \
    ChunkAllocator<Pal, DefaultArenaMap<Pal, DefaultPrimAlloc>>
#endif

  /**
   * The type of the default memory allocator.  This can be changed by defining
   * `SNMALLOC_DEFAULT_CHUNK_ALLOCATOR` before including this file.  By default
   * it is `ChunkAllocator<Pal>` a class that allocates directly from
   * the platform abstraction layer.
   */
  using GlobalVirtual = SNMALLOC_DEFAULT_CHUNK_ALLOCATOR;

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
    static T* alloc_meta(Args&&... args)
    {
      return default_memory_provider().template alloc_meta<T, alignment>(
        args...);
    }
  };
} // namespace snmalloc
