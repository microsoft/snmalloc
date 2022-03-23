#pragma once

#include "../ds/ptrwrap.h"
#include "../pal/pal_ds.h"
#include "largebuddyrange.h"

namespace snmalloc
{
  template<SNMALLOC_CONCEPT(RBRep) Rep>
  class RepList
  {
    uintptr_t head = 0;

    RepList(uintptr_t head) : head(head) {}

  public:
    constexpr RepList() = default;

    [[nodiscard]] bool is_empty() const
    {
      return head == 0;
    }

    RepList get_next()
    {
      SNMALLOC_ASSERT(!is_empty());
      auto next_field = &(Rep::ref(false, head));
      auto next = Rep::get(next_field);
      return {next};
    }

    capptr::Chunk<void> get_capability()
    {
      return capptr::Chunk<void>(reinterpret_cast<void*>(head));
    }

    RepList cons(capptr::Chunk<void> new_head_cap)
    {
      auto new_head = new_head_cap.unsafe_uintptr();
      auto field = &(Rep::ref(false, new_head));
      Rep::set(field, head);
      return {new_head};
    }

    template<typename F>
    void forall(F f)
    {
      auto curr = *this;
      while (!curr.is_empty())
      {
        auto next = curr.get_next();

        f(curr.get_capability());

        curr = next;
      }
    }
  };

  /**
   * Concurrent Stack
   *
   * This stack supports the following clients
   * (push|pop)* || pop_all* || ... || pop_all*
   *
   * That is a single thread that can do push and pop, and other threads
   * that do pop_all.  pop_all if it returns a value, returns all of the
   * stack, however, it may return nullptr if it races with either a push
   * or a pop.
   *
   * The primary use case is single-threaded access, where other threads
   * can attempt to steal all the values.
   */
  template<SNMALLOC_CONCEPT(RBRep) Rep>
  class RepStack
  {
    static constexpr auto empty = RepList<Rep>{};

  private:
    alignas(CACHELINE_SIZE) std::atomic<RepList<Rep>> stack{};

    RepList<Rep> take()
    {
      if (stack.load(std::memory_order_relaxed).is_empty())
        return empty;
      return stack.exchange(empty, std::memory_order_acquire);
    }

    void replace(RepList<Rep> new_head)
    {
      SNMALLOC_ASSERT(stack.load().is_empty());
      stack.store(new_head, std::memory_order_release);
    }

  public:
    constexpr RepStack() = default;

    void push(capptr::Chunk<void> new_head_cap)
    {
      auto old_head = take();
      auto new_head = old_head.cons(new_head_cap);
      replace(new_head);
    }

    capptr::Chunk<void> pop()
    {
      auto old_head = take();
      if (old_head.is_empty())
        return nullptr;

      auto next = old_head.get_next();
      replace(next);

      return old_head.get_capability();
    }

    RepList<Rep> pop_all()
    {
      return take();
    }
  };

  /**
   * This range slowly filters back memory to the parent range.
   * It locally caches memory and after it hasn't been used for some time
   * it goes back to its parent range.
   */

  template<typename ParentRange, typename PAL, typename Pagemap>
  class DecayRange
  {
    /**
     * How many slab sizes that can be provided.
     */
    static constexpr size_t NUM_SLAB_SIZES = Pal::address_bits - MIN_CHUNK_BITS;

    /**
     * Number of free stacks per chunk size that each allocator will use.
     * For performance ideally a power of 2.  We will return to the central
     * pool anything that has not be used in the last NUM_EPOCHS - 1, where
     * each epoch is separated by DecayMemoryTimerObject::PERIOD.
     * I.e. if period is 500ms and num of epochs is 4, then we will return to
     * the central pool anything not used for the last 1500-2000ms.
     */
    static constexpr size_t NUM_EPOCHS = 4;
    static_assert(bits::is_pow2(NUM_EPOCHS), "Code assumes power of two.");

    /**
     * Stack of ranges that have been returned for reuse.
     */
    ModArray<
      NUM_SLAB_SIZES,
      ModArray<NUM_EPOCHS, RepStack<BuddyChunkRep<Pagemap>>>>
      chunk_stack;

    typename ParentRange::State parent{};

    /**
     * Which is the current epoch to place dealloced chunks, and the
     * first place we look for allocating chunks.
     */
    static inline // alignas(CACHELINE_SIZE)
      std::atomic<size_t>
        epoch{0};

    /**
     * Flag to ensure one-shot registration with the PAL.
     */
    static inline std::atomic_bool registered_timer{false};

    std::atomic_bool registered_local{false};

    /**
     * All activated DecayRanges.
     */
    static inline std::atomic<DecayRange*> all_local{nullptr};

    DecayRange* all_local_next{nullptr};

    static void handle_decay_tick()
    {
      static_assert(
        ParentRange::ConcurrencySafe,
        "Parent must be concurrency safe, as dealloc_range is called here on "
        "potentially another thread's state.");
      auto new_epoch = (epoch + 1) % NUM_EPOCHS;
      // Flush old index for all threads.
      auto curr = all_local.load(std::memory_order_acquire);
      while (curr != nullptr)
      {
        for (size_t sc = 0; sc < NUM_SLAB_SIZES; sc++)
        {
          auto old_stack = curr->chunk_stack[sc][new_epoch].pop_all();

          old_stack.forall([curr, sc](auto cap) {
            curr->parent->dealloc_range(cap, MIN_CHUNK_SIZE << sc);
          });
        }
        curr = curr->all_local_next;
      }

      // Advance current index
      epoch = new_epoch;
    }

    class DecayMemoryTimerObject : public PalTimerObject
    {
      /***
       * Method for callback object to perform lazy decommit.
       */
      static void process(PalTimerObject*)
      {
#ifdef SNMALLOC_TRACING
        message<1024>("DecayRange::handle_decay_tick timer");
#endif
        handle_decay_tick();
      }

      // Specify that we notify the ChunkAllocator every 500ms.
      static constexpr size_t PERIOD = 500;

    public:
      constexpr DecayMemoryTimerObject() : PalTimerObject(&process, PERIOD) {}
    };

    static inline DecayMemoryTimerObject timer_object;

  public:
    class State
    {
      DecayRange commit_range{};

    public:
      constexpr State() = default;

      DecayRange* operator->()
      {
        return &commit_range;
      }
    };

    static constexpr bool Aligned = ParentRange::Aligned;

    static constexpr bool ConcurrencySafe = false;

    constexpr DecayRange() = default;

    capptr::Chunk<void> alloc_range(size_t size)
    {
      // Check local cache

      if constexpr (pal_supports<Time, PAL>)
      {
        auto slab_sizeclass = bits::next_pow2_bits(size) - MIN_CHUNK_BITS;
        // Try local cache of chunks first
        for (size_t e = 0; e < NUM_EPOCHS; e++)
        {
          auto p = chunk_stack[slab_sizeclass][(epoch - e) % NUM_EPOCHS].pop();

          if (p != nullptr)
          {
#ifdef SNMALLOC_TRACING
            message<1024>(
              "DecayRange::alloc_range: returning from local cache: {} on {}",
              address_cast(p),
              this);
#endif
            return p;
          }
        }
      }

      // Loop to possibly flush all the other local threads caches.
      // Note that flushing passes to the parent range, which may consolidate
      // blocks and thus be able to service this request.
      // Alternatively, we could implement stealing, but that wouldn't
      // be able to consolidate.
      capptr::Chunk<void> result;
      for (auto i = NUM_EPOCHS; i > 0; i--)
      {
        // Nothing in local cache, so allocate from parent.
        result = parent->alloc_range(size);
        if (result != nullptr)
        {
#ifdef SNMALLOC_TRACING
          message<1024>(
            "DecayRange::alloc_range: returning from parent: {} on {}",
            address_cast(result),
            this);
#endif
          return result;
        }

        // We have run out of memory.
        // Try to free some memory to the parent.
#ifdef SNMALLOC_TRACING
        message<1024>("DecayRange::handle_decay_tick OOM");
#endif
        handle_decay_tick();
      }

      // Last try.
      result = parent->alloc_range(size);

#ifdef SNMALLOC_TRACING
      message<1024>(
        "DecayRange::alloc_range: returning from parent last try: {} on {}",
        address_cast(result),
        this);
#endif

      return result;
    }

    void dealloc_range(capptr::Chunk<void> base, size_t size)
    {
      if constexpr (!pal_supports<Time, PAL>)
      {
        parent->dealloc_range(base, size);
        return;
      }

      if (!registered_timer.exchange(true))
      {
        // Register with the PAL.
        PAL::register_timer(&timer_object);
      }

      // Check we have registered
      if (!registered_local.exchange(true))
      {
        // Add to the list of local states.
        auto* head = all_local.load();
        do
        {
          all_local_next = head;
        } while (!all_local.compare_exchange_strong(head, this));
      }

      auto slab_sizeclass = bits::next_pow2_bits(size) - MIN_CHUNK_BITS;
      // Add to local cache.
#ifdef SNMALLOC_TRACING
      message<1024>(
        "DecayRange::dealloc_range: returning to local cache: {} on {}",
        address_cast(base),
        this);
#endif
      chunk_stack[slab_sizeclass][epoch].push(base);
    }
  };
} // namespace snmalloc
