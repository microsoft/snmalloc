#pragma once

#include "../mem/mem.h"

namespace snmalloc
{
  /**
   * Options for a specific snmalloc configuration.  Every globals object must
   * have one `constexpr` instance of this class called `Options`.  This should
   * be constructed to explicitly override any of the defaults.  A
   * configuration that does not need to override anything would simply declare
   * this as a field of the global object:
   *
   * ```c++
   * static constexpr snmalloc::Flags Options{};
   * ```
   *
   * A global configuration that wished to use out-of-line message queues but
   * accept the defaults for everything else would instead do this:
   *
   * ```c++
   *     static constexpr snmalloc::Flags Options{.IsQueueInline = false};
   * ```
   *
   * To maintain backwards source compatibility in future versions, any new
   * option added here should have its default set to be whatever snmalloc was
   * doing before the new option was added.
   */
  struct Flags
  {
    /**
     * Should allocators have inline message queues?  If this is true then
     * the `Allocator` is responsible for allocating the
     * `RemoteAllocator` that contains its message queue.  If this is false
     * then the `RemoteAllocator` must be separately allocated and provided
     * to the `Allocator` before it is used.
     */
    bool IsQueueInline = true;

    /**
     * Does the `Allocator` own a `Backend::LocalState` object?  If this is
     * true then the `Allocator` is responsible for allocating and
     * deallocating a local state object, otherwise the surrounding code is
     * responsible for creating it.
     */
    bool AllocOwnsLocalState = true;

    /**
     * Are `Allocator` allocated by the pool allocator?  If not then the
     * code embedding this snmalloc configuration is responsible for allocating
     * `Allocator` instances.
     */
    bool AllocIsPoolAllocated = true;

    /**
     * Are the front and back pointers to the message queue in a RemoteAllocator
     * considered to be capptr_bounds::Wildness::Tame (as opposed to Wild)?
     * That is, is it presumed that clients or other potentialadversaries cannot
     * access the front and back pointers themselves, even if they can access
     * the queue nodes themselves (which are always considered Wild)?
     */
    bool QueueHeadsAreTame = true;

    /**
     * Does the backend provide a capptr_domesticate function to sanity check
     * pointers? If so it will be called when untrusted pointers are consumed
     * (on dealloc and in freelists) otherwise a no-op version is provided.
     */
    bool HasDomesticate = false;
  };

  struct NoClientMetaDataProvider
  {
    using StorageType = Empty;
    using DataRef = Empty&;

    static size_t required_count(size_t)
    {
      return 1;
    }

    static DataRef get(StorageType* base, size_t)
    {
      return *base;
    }
  };

  template<typename T>
  struct ArrayClientMetaDataProvider
  {
    using StorageType = T;
    using DataRef = T&;

    static size_t required_count(size_t max_count)
    {
      return max_count;
    }

    static DataRef get(StorageType* base, size_t index)
    {
      return base[index];
    }
  };

  /**
   * Lazy variant of `ArrayClientMetaDataProvider<T>`.
   *
   * Reserves a single pointer of per-slab metadata footprint (the per-slab
   * overhead a full eager array would occupy is collapsed to one
   * `stl::Atomic<T*>`) and defers the construction of the underlying `T`
   * elements until `get` is first called for a given slab.
   *
   * Intended for `T` whose storage should not be paid for on slabs that are
   * never queried — for example, sampled heap-profiling metadata that is
   * touched only on a small fraction of allocations.  Per-slab footprint
   * before round-up is `sizeof(void*)` whether or not the slab is ever
   * profiled; the `slab_object_count * sizeof(T)` backing array is only
   * materialised on the first sampled touch.
   *
   * This primitive is not yet wired into any `Config`; consumers (the
   * frontend `FrontendSlabMetadata` and `globalalloc.h` callers) currently
   * invoke `ClientMeta::get(StorageType*, size_t)`.  Wiring this provider
   * up requires threading the per-slab object count from the pagemap entry
   * through `get_meta_for_object` to `get(StorageType*, size_t, size_t)`;
   * see Phase 3 for the integration work.
   *
   * `StorageType` is default-constructible (the atomic pointer is value-
   * initialised to null), matching the placement-new contracts in
   * `mem/metadata.h` and the `null_meta_store` fallback in
   * `global/globalalloc.h`.
   *
   * Lazy installation goes directly to the platform abstraction layer via
   * `DefaultPal::reserve` + `notify_using<YesZero>` rather than through the
   * frontend allocator, so it cannot recurse into user `malloc`.  Concurrent
   * first-touch is resolved by a double-checked compare-and-swap; the losing
   * thread decommits its temporary mapping via `notify_not_using`.  No
   * portable `Pal::release` exists, so the reservation itself is held for
   * the life of the slab.
   */
  template<typename T>
  struct LazyArrayClientMetaDataProvider
  {
    /**
     * Inline per-slab storage: one atomic pointer to the lazily-allocated
     * backing array.  Value-initialised to nullptr on construction so the
     * provider can detect "not yet materialised" with a single relaxed
     * load.  Sized to exactly one pointer; per Q1 we deliberately do not
     * cache the object count here (it is recovered from the pagemap
     * sizeclass and threaded through `get`).
     */
    struct StorageType
    {
      stl::Atomic<T*> backing{nullptr};
    };

    static_assert(
      sizeof(StorageType) == sizeof(void*),
      "LazyArrayClientMetaDataProvider::StorageType must be exactly one "
      "pointer wide");

    using DataRef = T&;

    /**
     * One slot of inline storage per slab regardless of the slab's object
     * count: the inline slot holds the atomic pointer to the lazily-
     * allocated backing array.  The frontend's
     * `get_client_storage_count` clamps this to a minimum of 1.
     */
    static constexpr size_t required_count(size_t /*max_count*/)
    {
      return 1;
    }

    /**
     * Round a byte count up to a multiple of the platform page size.
     * `DefaultPal::notify_using` requires page-aligned base and length
     * when zeroing, and `DefaultPal::reserve` always returns a
     * page-multiple region; the rounded size is used for both calls so
     * decommit on the CAS-loser path stays balanced.
     */
    static constexpr size_t round_to_page(size_t bytes)
    {
      return bits::align_up(bytes, DefaultPal::page_size);
    }

    /**
     * Slow-path: install a freshly zero-filled backing array for this
     * slab and publish it via release-store.  Double-checked CAS: if a
     * racing thread wins the publish, we decommit our temporary mapping
     * and observe the winner's pointer.
     *
     * On allocation failure or CAS-loss we deliberately do not call
     * `munmap`; there is no portable Pal `release`.  `notify_not_using`
     * returns the physical pages to the OS while leaving the (small)
     * virtual reservation in place.
     */
    SNMALLOC_SLOW_PATH static T* install(
      StorageType* base, size_t slab_object_count)
    {
      const size_t raw_bytes = slab_object_count * sizeof(T);
      const size_t alloc_bytes = round_to_page(raw_bytes);

      void* p = DefaultPal::reserve(alloc_bytes);
      if (SNMALLOC_UNLIKELY(p == nullptr))
        return nullptr;

      // YesZero so `T` slots are observably zero on first read; on POSIX
      // this is typically free for fresh mappings, on Windows this also
      // commits the pages.
      if (SNMALLOC_UNLIKELY(
            !DefaultPal::template notify_using<YesZero>(p, alloc_bytes)))
        return nullptr;

      auto* fresh = static_cast<T*>(p);
      T* expected = nullptr;
      if (base->backing.compare_exchange_strong(
            expected,
            fresh,
            stl::memory_order_acq_rel,
            stl::memory_order_acquire))
      {
        return fresh;
      }

      // Lost the race: decommit our temporary mapping and return the
      // winner's pointer.  Reservation is intentionally leaked (no
      // portable Pal::release).
      DefaultPal::notify_not_using(p, alloc_bytes);
      return expected;
    }

    /**
     * Per-object accessor.  Threads the per-slab object count through so
     * the lazy install can size the backing array; callers obtain the
     * count from the pagemap `MetaEntry` via
     * `sizeclass_to_slab_object_count(entry.get_sizeclass())`.
     *
     * This signature is a deliberate extension of the structural
     * `ClientMeta::get(StorageType*, size_t)` contract honoured by
     * `NoClientMetaDataProvider` and `ArrayClientMetaDataProvider`.
     * Wiring this provider into a `Config` (Phase 3) requires extending
     * `FrontendSlabMetadata::get_meta_for_object` to forward the count.
     */
    static DataRef
    get(StorageType* base, size_t index, size_t slab_object_count)
    {
      T* buf = base->backing.load(stl::memory_order_acquire);
      if (SNMALLOC_UNLIKELY(buf == nullptr))
        buf = install(base, slab_object_count);
      return buf[index];
    }
  };

  /**
   * Class containing definitions that are likely to be used by all except for
   * the most unusual back-end implementations.  This can be subclassed as a
   * convenience for back-end implementers, but is not required.
   */
  class CommonConfig
  {
  public:
    /**
     * Special remote that should never be used as a real remote.
     * This is used to initialise allocators that should always hit the
     * remote path for deallocation. Hence moving a branch off the critical
     * path.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static RemoteAllocator unused_remote;
  };

  template<typename PAL>
  static constexpr size_t MinBaseSizeBits()
  {
    if constexpr (pal_supports<AlignedAllocation, PAL>)
    {
      return bits::next_pow2_bits_const(PAL::minimum_alloc_size);
    }
    else
    {
      return MIN_CHUNK_BITS;
    }
  }
} // namespace snmalloc

#include "../mem/remotecache.h"
