#pragma once

#include "../ds/ds.h"
#include "../pal/pal_ds.h"
#include "metadata.h"
#include "sizeclasstable.h"

namespace snmalloc
{
  /**
   * Frontend cache for large object allocations.
   *
   * This cache sits in the per-thread Allocator and intercepts large
   * alloc/dealloc before they reach the backend. By caching recently freed
   * large objects, we avoid:
   *   - Pagemap writes on dealloc (clearing N entries) and alloc (setting N
   *     entries)
   *   - Metadata allocation/deallocation
   *   - Buddy allocator tree operations
   *   - Decommit/recommit syscalls (if DecayRange is also in the pipeline)
   *
   * The cache uses the slab metadata's SeqSet node to link cached entries,
   * storing no data inside the freed object itself. The chunk address is
   * recovered from the metadata's free_queue, and the chunk size from the
   * pagemap entry's sizeclass.
   *
   * Epoch rotation is driven by a PAL timer (DecayMemoryTimerObject).
   * A global epoch counter is advanced periodically by the timer. Each
   * cache instance tracks the last epoch it observed and self-flushes
   * stale epochs on its next operation. This means no concurrent access
   * to the per-thread SeqSets is needed.
   *
   * Each sizeclass has an adaptive budget that bounds how many items can
   * be cached. The budget starts at 1 and adjusts on each epoch rotation:
   *   - If stale entries were flushed (surplus), decrease budget.
   *   - If no entries were flushed and the cache was actively drained by
   *     allocations (not just empty from startup), increase budget.
   * This allows the cache to grow to match the working set while shrinking
   * when the workload subsides.
   *
   * Template parameter Config provides Backend, PagemapEntry, Pal, etc.
   */
  template<typename Config>
  class LargeObjectCache
  {
    using PAL = typename Config::Pal;
    using BackendSlabMetadata = typename Config::Backend::SlabMetadata;
    using PagemapEntry = typename Config::PagemapEntry;

    /**
     * Maximum chunk size we cache (4 MiB).
     * Larger allocations bypass the cache and go directly to/from backend.
     */
    static constexpr size_t MAX_CACHEABLE_SIZE =
      bits::one_at_bit(22);

    /**
     * Number of chunk sizeclasses we actually cache.
     * Covers all fine-grained large classes up to MAX_CACHEABLE_SIZE.
     * This is the large class index of MAX_CACHEABLE_SIZE, plus one.
     */
    static constexpr size_t NUM_SIZECLASSES =
      size_to_large_class_index_const(MAX_CACHEABLE_SIZE) + 1;

    /**
     * Number of epoch slots for cached ranges.
     * Must be a power of 2.
     */
    static constexpr size_t NUM_EPOCHS = 4;
    static_assert(bits::is_pow2(NUM_EPOCHS));

    /**
     * Global epoch counter, advanced by the timer callback.
     * All LargeObjectCache instances read this to detect when epochs
     * have advanced and stale entries need flushing.
     */
    static inline stl::Atomic<size_t> global_epoch{0};

    /**
     * Timer callback that advances the global epoch.
     */
    class DecayMemoryTimerObject : public PalTimerObject
    {
      static void process(PalTimerObject*)
      {
        auto e = global_epoch.load(stl::memory_order_relaxed);
        global_epoch.store(e + 1, stl::memory_order_release);
      }

      /// Timer fires every 500ms.
      static constexpr size_t PERIOD = 500;

    public:
      constexpr DecayMemoryTimerObject() : PalTimerObject(&process, PERIOD) {}
    };

    static inline DecayMemoryTimerObject timer_object;

    /**
     * Flag to ensure one-shot timer registration.
     */
    static inline stl::AtomicBool registered_timer{false};

    /**
     * Per-sizeclass adaptive budget state.
     */
    struct SizeclassState
    {
      /// Maximum number of items allowed in the cache for this sizeclass.
      /// Starts at 1 so the first deallocation is always cached.
      size_t budget{1};

      /// Current number of cached items across all epoch slots.
      size_t count{0};

      /// Number of cache misses since last cache insert.
      /// Reset to 0 each time we successfully add to the cache.
      size_t misses{0};

      /// Peak value of misses this epoch.
      /// This is what we use for budget growth - it captures the maximum
      /// "depth" of consecutive misses, not cumulative misses.
      size_t peak_misses{0};
    };

    /**
     * Per-sizeclass budget tracking.
     */
    ModArray<NUM_SIZECLASSES, SizeclassState> sc_state;

    /**
     * Per-sizeclass, per-epoch SeqSets of cached metadata.
     * Indexed as lists[sizeclass_index][epoch % NUM_EPOCHS].
     */
    ModArray<NUM_SIZECLASSES, ModArray<NUM_EPOCHS, SeqSet<BackendSlabMetadata>>>
      lists;

    /**
     * The epoch this instance last synced to.
     * Used to detect when new epochs have passed and old ones need flushing.
     */
    size_t local_epoch{0};

    /**
     * Convert a sizeclass_t to the cache index.
     * Uses sizeclass_t::as_large() to extract the sequential large
     * class index directly, avoiding any size-to-index computation.
     */
    static size_t to_sizeclass(sizeclass_t sc)
    {
      return sc.as_large();
    }

    /**
     * Check if a sizeclass is cacheable.
     * Only sizeclasses whose large class index fits within
     * NUM_SIZECLASSES (up to MAX_CACHEABLE_SIZE) are cached;
     * larger allocations bypass the cache entirely.
     */
    static bool is_cacheable(sizeclass_t sc)
    {
      return sc.as_large() < NUM_SIZECLASSES;
    }

    /**
     * Register the global timer if not already done.
     */
    void ensure_registered()
    {
      if constexpr (pal_supports<Time, PAL>)
      {
        if (
          !registered_timer.load(stl::memory_order_relaxed) &&
          !registered_timer.exchange(true, stl::memory_order_acq_rel))
        {
          PAL::register_timer(&timer_object);
        }
      }
    }

    /**
     * Catch up to the global epoch, flushing any stale epochs and
     * adjusting per-sizeclass budgets.
     */
    template<typename FlushFn>
    void sync_epoch(FlushFn&& flush_fn)
    {
      if constexpr (pal_supports<Time, PAL>)
      {
        auto current = global_epoch.load(stl::memory_order_acquire);

        auto behind = current - local_epoch;
        if (behind == 0)
          return;

        if (behind > NUM_EPOCHS)
          behind = NUM_EPOCHS;

        // Snapshot counts before flushing.
        size_t before_count[NUM_SIZECLASSES];
        for (size_t sc = 0; sc < NUM_SIZECLASSES; sc++)
          before_count[sc] = sc_state[sc].count;

        // Flush stale epoch slots.
        for (size_t i = 0; i < behind; i++)
        {
          auto epoch_to_flush = (local_epoch + 1 + i) % NUM_EPOCHS;
          flush_epoch_slot(epoch_to_flush, flush_fn);
        }

        // Adjust budgets based on what happened.
        // Net out misses against flushed items to determine direction.
        for (size_t sc = 0; sc < NUM_SIZECLASSES; sc++)
        {
          auto& state = sc_state[sc];
          size_t flushed = before_count[sc] - state.count;

          if (state.peak_misses > flushed)
          {
            // More misses than surplus: grow budget by the difference.
            state.budget += state.peak_misses - flushed;
          }
          else if (flushed > state.peak_misses)
          {
            // More surplus than misses: shrink budget smoothly.
            state.budget -= (flushed - state.peak_misses) / 2;
          }
          // If equal, budget stays the same.

          state.misses = 0;
          state.peak_misses = 0;
        }

        local_epoch = current;
      }
    }

    /**
     * Flush all entries in a single epoch slot.
     * Decrements per-sizeclass counts.
     */
    template<typename FlushFn>
    void flush_epoch_slot(size_t epoch_slot, FlushFn&& flush_fn)
    {
      for (size_t sc = 0; sc < NUM_SIZECLASSES; sc++)
      {
        auto& list = lists[sc][epoch_slot];
        while (!list.is_empty())
        {
          sc_state[sc].count--;
          flush_fn(list.pop_front());
        }
      }
    }

  public:
    constexpr LargeObjectCache() = default;

    /**
     * Try to satisfy a large allocation from the cache.
     *
     * @param sc         The sizeclass of the allocation.
     * @param flush_fn   Callback to flush stale entries during epoch sync.
     * @return Metadata for a cached chunk, or nullptr on cache miss.
     */
    template<typename FlushFn>
    BackendSlabMetadata* try_alloc(sizeclass_t sc, FlushFn&& flush_fn)
    {
      // Don't cache very large allocations.
      if (!is_cacheable(sc))
        return nullptr;

      sync_epoch(flush_fn);

      auto idx = to_sizeclass(sc);
      auto current = local_epoch;

      // Check current epoch first, then older ones.
      for (size_t age = 0; age < NUM_EPOCHS; age++)
      {
        auto& list = lists[idx][(current - age) % NUM_EPOCHS];
        if (!list.is_empty())
        {
          sc_state[idx].count--;
          return list.pop_front();
        }
      }

      // Cache miss - track for budget growth.
      sc_state[idx].misses++;
      if (sc_state[idx].misses > sc_state[idx].peak_misses)
        sc_state[idx].peak_misses = sc_state[idx].misses;
      return nullptr;
    }

    /**
     * Cache a large deallocation.
     *
     * If the sizeclass is at its budget, the entry is flushed immediately
     * instead of being cached.
     *
     * @param meta        The slab metadata for the chunk.
     * @param sc          The sizeclass of the allocation.
     * @param flush_fn    Callback to flush stale entries during epoch sync,
     *                    and to flush this entry if over budget.
     */
    template<typename FlushFn>
    void cache(BackendSlabMetadata* meta, sizeclass_t sc, FlushFn&& flush_fn)
    {
      // Don't cache very large allocations - flush directly to backend.
      if (!is_cacheable(sc))
      {
        flush_fn(meta);
        return;
      }

      ensure_registered();
      sync_epoch(flush_fn);

      auto idx = to_sizeclass(sc);

      if (sc_state[idx].count >= sc_state[idx].budget)
      {
        // Over budget: flush immediately rather than caching.
        flush_fn(meta);
        return;
      }

      sc_state[idx].count++;
      sc_state[idx].misses = 0; // Reset miss counter on successful cache.
      lists[idx][local_epoch % NUM_EPOCHS].insert(meta);
    }

    /**
     * Flush all cached entries back to the backend.
     * Called during allocator teardown/flush.
     */
    template<typename FlushFn>
    void flush_all(FlushFn&& flush_fn)
    {
      for (size_t e = 0; e < NUM_EPOCHS; e++)
      {
        flush_epoch_slot(e, flush_fn);
      }
    }

    /**
     * Flush all cached entries with sizeclass strictly smaller than
     * the given chunk_size. These can coalesce in the buddy allocator
     * to form the needed size.
     *
     * @return true if any entries were flushed.
     */
    template<typename FlushFn>
    bool flush_smaller(sizeclass_t sc, FlushFn&& flush_fn)
    {
      // If not cacheable, all cached entries are smaller.
      size_t target_idx = is_cacheable(sc) ?
        to_sizeclass(sc) :
        NUM_SIZECLASSES;
      bool flushed = false;
      for (size_t i = 0; i < target_idx; i++)
      {
        for (size_t e = 0; e < NUM_EPOCHS; e++)
        {
          auto& list = lists[i][e];
          while (!list.is_empty())
          {
            sc_state[i].count--;
            flush_fn(list.pop_front());
            flushed = true;
          }
        }
      }
      return flushed;
    }

    /**
     * Flush a single cached entry with sizeclass >= the given sizeclass.
     * The buddy allocator can split this to satisfy the request.
     *
     * @return true if an entry was flushed.
     */
    template<typename FlushFn>
    bool flush_one_larger(sizeclass_t sc, FlushFn&& flush_fn)
    {
      // Nothing in cache can satisfy requests larger than our max.
      if (!is_cacheable(sc))
        return false;

      auto target_idx = to_sizeclass(sc);
      for (size_t i = target_idx; i < NUM_SIZECLASSES; i++)
      {
        for (size_t e = 0; e < NUM_EPOCHS; e++)
        {
          auto& list = lists[i][e];
          if (!list.is_empty())
          {
            sc_state[i].count--;
            flush_fn(list.pop_front());
            return true;
          }
        }
      }
      return false;
    }

    /**
     * Check if the cache is completely empty.
     */
    bool is_empty() const
    {
      for (size_t sc = 0; sc < NUM_SIZECLASSES; sc++)
      {
        for (size_t e = 0; e < NUM_EPOCHS; e++)
        {
          if (!lists[sc][e].is_empty())
            return false;
        }
      }
      return true;
    }
  };
} // namespace snmalloc
