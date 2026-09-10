#pragma once

#include "../ds/ds.h"
#include "../mem/mem.h"
#include "empty_range.h"
#include "largebuddyrange.h"
#include "range_helpers.h"

namespace snmalloc
{
  /**
   * Intrusive singly-linked list using pagemap entries for storage.
   *
   * This uses BuddyChunkRep's pagemap entry access (direction=false, i.e.
   * Word::Two) to store the "next" pointer for each node.
   */
  template<SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap>
  class DecayList
  {
    using Rep = BuddyChunkRep<Pagemap>;

    uintptr_t head = 0;

    DecayList(uintptr_t head) : head(head) {}

  public:
    constexpr DecayList() = default;

    [[nodiscard]] bool is_empty() const
    {
      return head == 0;
    }

    DecayList get_next()
    {
      SNMALLOC_ASSERT(!is_empty());
      auto next_field = Rep::ref(false, head);
      auto next = Rep::get(next_field);
      return {next};
    }

    capptr::Arena<void> get_capability()
    {
      return capptr::Arena<void>::unsafe_from(reinterpret_cast<void*>(head));
    }

    DecayList cons(capptr::Arena<void> new_head_cap)
    {
      auto new_head = new_head_cap.unsafe_uintptr();
      auto field = Rep::ref(false, new_head);
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
   * Concurrent stack for caching deallocated ranges.
   *
   * Supports the following concurrency pattern:
   *   (push|pop)* || pop_all* || ... || pop_all*
   *
   * That is, a single thread can do push and pop, and other threads
   * can do pop_all. pop_all returns all of the stack if it doesn't
   * race, or empty if it does.
   *
   * The primary use case is single-threaded access, where other threads
   * can attempt to drain all values (via the timer callback).
   */
  template<SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap>
  class DecayStack
  {
    static constexpr auto empty = DecayList<Pagemap>{};

    alignas(CACHELINE_SIZE) stl::Atomic<DecayList<Pagemap>> stack{};

    DecayList<Pagemap> take()
    {
      if (stack.load(stl::memory_order_relaxed).is_empty())
        return empty;
      return stack.exchange(empty, stl::memory_order_acquire);
    }

    void replace(DecayList<Pagemap> new_head)
    {
      SNMALLOC_ASSERT(stack.load().is_empty());
      stack.store(new_head, stl::memory_order_release);
    }

  public:
    constexpr DecayStack() = default;

    void push(capptr::Arena<void> new_head_cap)
    {
      auto old_head = take();
      auto new_head = old_head.cons(new_head_cap);
      replace(new_head);
    }

    capptr::Arena<void> pop()
    {
      auto old_head = take();
      if (old_head.is_empty())
        return nullptr;

      auto next = old_head.get_next();
      replace(next);

      return old_head.get_capability();
    }

    DecayList<Pagemap> pop_all()
    {
      return take();
    }
  };

  /**
   * A range that provides temporal caching of deallocated ranges.
   *
   * Instead of immediately releasing deallocated memory back to the parent
   * range (which would decommit it), this range caches it locally and
   * uses PAL timers to gradually release it. This avoids expensive
   * repeated decommit/recommit cycles for transient allocation patterns
   * (e.g. repeatedly allocating and deallocating ~800KB objects).
   *
   * The range uses an epoch-based rotation scheme:
   *   - Deallocated ranges are placed in the current epoch's stack
   *   - A timer periodically advances the epoch
   *   - When the epoch advances, the oldest epoch's entries are flushed
   *     to the parent range
   *
   * The parent range MUST be ConcurrencySafe, as the timer callback may
   * flush entries from a different thread context.
   *
   * PAL - Platform abstraction layer (for timer support)
   * Pagemap - Used for storing linked list nodes in pagemap entries
   */
  template<typename PAL, SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap>
  struct DecayRange
  {
    template<typename ParentRange = EmptyRange<>>
    class Type : public ContainsParent<ParentRange>
    {
      using ContainsParent<ParentRange>::parent;

    public:
      static constexpr bool Aligned = ParentRange::Aligned;

      static constexpr bool ConcurrencySafe = false;

      using ChunkBounds = typename ParentRange::ChunkBounds;

    private:
      /**
       * Maximum chunk size bits we cache (4 MiB = 2^22).
       */
      static constexpr size_t MAX_CACHEABLE_BITS = 22;

      /**
       * Maximum chunk size we cache (4 MiB).
       * Larger allocations bypass the cache and go directly to/from parent.
       */
      static constexpr size_t MAX_CACHEABLE_SIZE =
        bits::one_at_bit(MAX_CACHEABLE_BITS);

      /**
       * How many slab sizes that can be cached.
       * Only covers sizes from MIN_CHUNK_SIZE up to MAX_CACHEABLE_SIZE.
       */
      static constexpr size_t NUM_SLAB_SIZES =
        MAX_CACHEABLE_BITS - MIN_CHUNK_BITS + 1;

      /**
       * Number of epoch slots for cached ranges.
       *
       * Ranges not used within (NUM_EPOCHS - 1) timer periods will be
       * released to the parent. E.g., with period=500ms and NUM_EPOCHS=4,
       * memory not reused within 1500-2000ms will be released.
       *
       * Must be a power of 2.
       */
      static constexpr size_t NUM_EPOCHS = 4;
      static_assert(bits::is_pow2(NUM_EPOCHS), "NUM_EPOCHS must be power of 2");

      /**
       * Per-sizeclass, per-epoch stacks of cached ranges.
       */
      ModArray<NUM_SLAB_SIZES, ModArray<NUM_EPOCHS, DecayStack<Pagemap>>>
        chunk_stack;

      /**
       * Current epoch index.
       */
      static inline stl::Atomic<size_t> epoch{0};

      /**
       * Flag to ensure one-shot timer registration with the PAL.
       */
      static inline stl::AtomicBool registered_timer{false};

      /**
       * Flag indicating this instance has been registered in the global list.
       */
      stl::AtomicBool registered_local{false};

      /**
       * Global list of all activated DecayRange instances.
       * Used by the timer to iterate and flush old entries.
       */
      static inline stl::Atomic<Type*> all_local{nullptr};

      /**
       * Next pointer for the global intrusive list.
       */
      Type* all_local_next{nullptr};

      /**
       * Flush the oldest epoch's entries across all instances
       * and advance the epoch.
       */
      static void handle_decay_tick()
      {
        static_assert(
          ParentRange::ConcurrencySafe,
          "Parent range must be concurrency safe, as dealloc_range is called "
          "from the timer callback on a potentially different thread.");

        auto new_epoch =
          (epoch.load(stl::memory_order_relaxed) + 1) % NUM_EPOCHS;

        // Flush the epoch that is about to become current
        // across all registered instances.
        auto curr = all_local.load(stl::memory_order_acquire);
        while (curr != nullptr)
        {
          for (size_t sc = 0; sc < NUM_SLAB_SIZES; sc++)
          {
            auto old_stack = curr->chunk_stack[sc][new_epoch].pop_all();

            old_stack.forall([curr, sc](auto cap) {
              size_t size = MIN_CHUNK_SIZE << sc;
#ifdef SNMALLOC_TRACING
              message<1024>(
                "DecayRange::tick flushing {} size {} to parent",
                cap.unsafe_ptr(),
                size);
#endif
              curr->parent.dealloc_range(cap, size);
            });
          }
          curr = curr->all_local_next;
        }

        // Advance the epoch
        epoch.store(new_epoch, stl::memory_order_release);
      }

      /**
       * Timer callback object for periodic decay.
       */
      class DecayMemoryTimerObject : public PalTimerObject
      {
        static void process(PalTimerObject*)
        {
#ifdef SNMALLOC_TRACING
          message<1024>("DecayRange::handle_decay_tick timer");
#endif
          handle_decay_tick();
        }

        /// Timer fires every 500ms.
        static constexpr size_t PERIOD = 500;

      public:
        constexpr DecayMemoryTimerObject() : PalTimerObject(&process, PERIOD) {}
      };

      static inline DecayMemoryTimerObject timer_object;

      void ensure_registered()
      {
        // Register the global timer if this is the first instance.
        if (
          !registered_timer.load(stl::memory_order_relaxed) &&
          !registered_timer.exchange(true, stl::memory_order_acq_rel))
        {
          PAL::register_timer(&timer_object);
        }

        // Register this instance in the global list.
        if (
          !registered_local.load(stl::memory_order_relaxed) &&
          !registered_local.exchange(true, stl::memory_order_acq_rel))
        {
          auto* head = all_local.load(stl::memory_order_relaxed);
          do
          {
            all_local_next = head;
          } while (!all_local.compare_exchange_weak(
            head, this, stl::memory_order_release, stl::memory_order_relaxed));
        }
      }

    public:
      constexpr Type() = default;

      CapPtr<void, ChunkBounds> alloc_range(size_t size)
      {
        SNMALLOC_ASSERT(bits::is_pow2(size));
        SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);

        auto slab_sizeclass = bits::next_pow2_bits(size) - MIN_CHUNK_BITS;

        // Bypass cache for sizes beyond what we track.
        if (slab_sizeclass >= NUM_SLAB_SIZES)
          return parent.alloc_range(size);

        if constexpr (pal_supports<Time, PAL>)
        {
          // Try local cache across all epochs, starting from current.
          auto current_epoch = epoch.load(stl::memory_order_relaxed);
          for (size_t e = 0; e < NUM_EPOCHS; e++)
          {
            auto p =
              chunk_stack[slab_sizeclass][(current_epoch - e) % NUM_EPOCHS]
                .pop();

            if (p != nullptr)
            {
#ifdef SNMALLOC_TRACING
              message<1024>(
                "DecayRange::alloc_range returning {} from local cache",
                p.unsafe_ptr());
#endif
              return p;
            }
          }
        }

        // Try parent. If OOM, flush decay caches and retry.
        CapPtr<void, ChunkBounds> result;
        for (size_t i = NUM_EPOCHS; i > 0; i--)
        {
          result = parent.alloc_range(size);
          if (result != nullptr)
          {
#ifdef SNMALLOC_TRACING
            message<1024>(
              "DecayRange::alloc_range returning {} from parent",
              result.unsafe_ptr());
#endif
            return result;
          }

          // OOM: force-flush decay caches to free memory.
#ifdef SNMALLOC_TRACING
          message<1024>("DecayRange::alloc_range OOM, flushing decay caches");
#endif
          handle_decay_tick();
        }

        // Final attempt after flushing all epochs.
        result = parent.alloc_range(size);
#ifdef SNMALLOC_TRACING
        message<1024>(
          "DecayRange::alloc_range final attempt: {}", result.unsafe_ptr());
#endif
        return result;
      }

      void dealloc_range(CapPtr<void, ChunkBounds> base, size_t size)
      {
        SNMALLOC_ASSERT(bits::is_pow2(size));
        SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);

        auto slab_sizeclass = bits::next_pow2_bits(size) - MIN_CHUNK_BITS;

        // Bypass cache for sizes beyond what we track.
        if (slab_sizeclass >= NUM_SLAB_SIZES)
        {
          parent.dealloc_range(base, size);
          return;
        }

        if constexpr (pal_supports<Time, PAL>)
        {
          ensure_registered();

#ifdef SNMALLOC_TRACING
          message<1024>(
            "DecayRange::dealloc_range caching {} size {}",
            base.unsafe_ptr(),
            size);
#endif
          auto current_epoch = epoch.load(stl::memory_order_relaxed);
          chunk_stack[slab_sizeclass][current_epoch].push(base);
        }
        else
        {
          // No timer support: pass through directly.
          parent.dealloc_range(base, size);
        }
      }
    };
  };
} // namespace snmalloc
