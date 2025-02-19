#pragma once

#include "snmalloc/aal/address.h"
#include "snmalloc/mem/remoteallocator.h"
#include "snmalloc/mem/secondary.h"
#if defined(_MSC_VER)
#  define ALLOCATOR __declspec(allocator) __declspec(restrict)
#elif __has_attribute(malloc)
#  define ALLOCATOR __attribute__((malloc))
#else
#  define ALLOCATOR
#endif

#include "../ds/ds.h"
#include "corealloc.h"
#include "freelist.h"
#include "localcache.h"
#include "pool.h"
#include "remotecache.h"
#include "sizeclasstable.h"
#include "snmalloc/stl/utility.h"

#include <string.h>

namespace snmalloc
{
  /**
   * A local allocator contains the fast-path allocation routines and
   * encapsulates all of the behaviour of an allocator that is local to some
   * context, typically a thread.  This delegates to a `CoreAllocator` for all
   * slow-path operations, including anything that requires claiming new chunks
   * of address space.
   *
   * The template parameter defines the configuration of this allocator and is
   * passed through to the associated `CoreAllocator`.  The `Options` structure
   * of this defines one property that directly affects the behaviour of the
   * local allocator: `LocalAllocSupportsLazyInit`, which defaults to true,
   * defines whether the local allocator supports lazy initialisation.  If this
   * is true then the local allocator will construct a core allocator the first
   * time it needs to perform a slow-path operation.  If this is false then the
   * core allocator must be provided externally by invoking the `init` method
   * on this class *before* any allocation-related methods are called.
   */
  template<SNMALLOC_CONCEPT(IsConfig) Config_>
  class LocalAllocator
  {
  public:
    using Config = Config_;

  private:
    /**
     * Define local names for specialised versions of various types that are
     * specialised for the back-end that we are using.
     * @{
     */
    using CoreAlloc = CoreAllocator<Config>;
    using PagemapEntry = typename Config::PagemapEntry;
    /// }@

    // Free list per small size class.  These are used for
    // allocation on the fast path. This part of the code is inspired by
    // mimalloc.
    // Also contains remote deallocation cache.
    LocalCache<Config> local_cache{&Config::unused_remote};

    // Underlying allocator for most non-fast path operations.
    CoreAlloc* core_alloc{nullptr};

    // As allocation and deallocation can occur during thread teardown
    // we need to record if we are already in that state as we will not
    // receive another teardown call, so each operation needs to release
    // the underlying data structures after the call.
    bool post_teardown{false};

    /**
     * Checks if the core allocator has been initialised, and runs the
     * `action` with the arguments, args.
     *
     * If the core allocator is not initialised, then first initialise it,
     * and then perform the action using the core allocator.
     *
     * This is an abstraction of the common pattern of check initialisation,
     * and then performing the operations.  It is carefully crafted to tail
     * call the continuations, and thus generate good code for the fast path.
     */
    template<typename Action, typename... Args>
    SNMALLOC_FAST_PATH decltype(auto) check_init(Action action, Args... args)
    {
      if (SNMALLOC_LIKELY(core_alloc != nullptr))
      {
        return core_alloc->handle_message_queue(action, core_alloc, args...);
      }
      return lazy_init(action, args...);
    }

    /**
     * This initialises the fast allocator by acquiring a core allocator, and
     * setting up its local copy of data structures.
     *
     * If the allocator does not support lazy initialisation then this assumes
     * that initialisation has already taken place and invokes the action
     * immediately.
     */
    template<typename Action, typename... Args>
    SNMALLOC_SLOW_PATH decltype(auto) lazy_init(Action action, Args... args)
    {
      SNMALLOC_ASSERT(core_alloc == nullptr);
      if constexpr (!Config::Options.LocalAllocSupportsLazyInit)
      {
        SNMALLOC_CHECK(
          false &&
          "lazy_init called on an allocator that doesn't support lazy "
          "initialisation");
        // Unreachable, but needed to keep the type checker happy in deducing
        // the return type of this function.
        return static_cast<decltype(action(core_alloc, args...))>(nullptr);
      }
      else
      {
        // Initialise the thread local allocator
        if constexpr (Config::Options.CoreAllocOwnsLocalState)
        {
          init();
        }

        // register_clean_up must be called after init.  register clean up may
        // be implemented with allocation, so need to ensure we have a valid
        // allocator at this point.
        if (!post_teardown)
          // Must be called at least once per thread.
          // A pthread implementation only calls the thread destruction handle
          // if the key has been set.
          Config::register_clean_up();

        // Perform underlying operation
        auto r = action(core_alloc, args...);

        // After performing underlying operation, in the case of teardown
        // already having begun, we must flush any state we just acquired.
        if (post_teardown)
        {
#ifdef SNMALLOC_TRACING
          message<1024>("post_teardown flush()");
#endif
          // We didn't have an allocator because the thread is being torndown.
          // We need to return any local state, so we don't leak it.
          flush();
        }

        return r;
      }
    }

    /**
     * Allocation that are larger than are handled by the fast allocator must be
     * passed to the core allocator.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH capptr::Alloc<void> alloc_not_small(size_t size)
    {
      if (size == 0)
      {
        // Deal with alloc zero of with a small object here.
        // Alternative semantics giving nullptr is also allowed by the
        // standard.
        return small_alloc<NoZero>(1);
      }

      return check_init([&](CoreAlloc* core_alloc) {
        if (size > bits::one_at_bit(bits::BITS - 1))
        {
          // Cannot allocate something that is more that half the size of the
          // address space
          errno = ENOMEM;
          return capptr::Alloc<void>{nullptr};
        }

        // Check if secondary allocator wants to offer the memory
        void* result =
          SecondaryAllocator::allocate([size]() -> stl::Pair<size_t, size_t> {
            return {size, natural_alignment(size)};
          });
        if (result != nullptr)
          return capptr::Alloc<void>::unsafe_from(result);

        // Grab slab of correct size
        // Set remote as large allocator remote.
        auto [chunk, meta] = Config::Backend::alloc_chunk(
          core_alloc->get_backend_local_state(),
          large_size_to_chunk_size(size),
          PagemapEntry::encode(
            core_alloc->public_state(), size_to_sizeclass_full(size)),
          size_to_sizeclass_full(size));
        // set up meta data so sizeclass is correct, and hence alloc size, and
        // external pointer.
#ifdef SNMALLOC_TRACING
        message<1024>("size {} pow2size {}", size, bits::next_pow2_bits(size));
#endif

        // Initialise meta data for a successful large allocation.
        if (meta != nullptr)
        {
          meta->initialise_large(
            address_cast(chunk), freelist::Object::key_root);
          core_alloc->laden.insert(meta);
        }

        if (zero_mem == YesZero && chunk.unsafe_ptr() != nullptr)
        {
          Config::Pal::template zero<false>(
            chunk.unsafe_ptr(), bits::next_pow2(size));
        }

        return capptr_chunk_is_alloc(capptr_to_user_address_control(chunk));
      });
    }

    template<ZeroMem zero_mem>
    SNMALLOC_FAST_PATH capptr::Alloc<void> small_alloc(size_t size)
    {
      auto domesticate =
        [this](freelist::QueuePtr p) SNMALLOC_FAST_PATH_LAMBDA {
          return capptr_domesticate<Config>(core_alloc->backend_state_ptr(), p);
        };
      auto slowpath = [&](
                        smallsizeclass_t sizeclass,
                        freelist::Iter<>* fl) SNMALLOC_FAST_PATH_LAMBDA {
        if (SNMALLOC_LIKELY(core_alloc != nullptr))
        {
          return core_alloc->handle_message_queue(
            [](
              CoreAlloc* core_alloc,
              smallsizeclass_t sizeclass,
              freelist::Iter<>* fl) {
              return core_alloc->template small_alloc<zero_mem>(sizeclass, *fl);
            },
            core_alloc,
            sizeclass,
            fl);
        }
        return lazy_init(
          [&](CoreAlloc*, smallsizeclass_t sizeclass) {
            return small_alloc<zero_mem>(sizeclass_to_size(sizeclass));
          },
          sizeclass);
      };

      return local_cache.template alloc<zero_mem>(domesticate, size, slowpath);
    }

    /**
     * Slow path for deallocation we do not have space for this remote
     * deallocation. This could be because,
     *   - we actually don't have space for this remote deallocation,
     *     and need to send them on; or
     *   - the allocator was not already initialised.
     * In the second case we need to recheck if this is a remote deallocation,
     * as we might acquire the originating allocator.
     */
    SNMALLOC_SLOW_PATH void
    dealloc_remote_slow(const PagemapEntry& entry, capptr::Alloc<void> p)
    {
      if (core_alloc != nullptr)
      {
#ifdef SNMALLOC_TRACING
        message<1024>(
          "Remote dealloc post {} ({}, {})",
          p.unsafe_ptr(),
          alloc_size(p.unsafe_ptr()),
          address_cast(entry.get_slab_metadata()));
#endif
        local_cache.remote_dealloc_cache.template dealloc<sizeof(CoreAlloc)>(
          entry.get_slab_metadata(), p, &local_cache.entropy);

        core_alloc->post();
        return;
      }

      // Recheck what kind of dealloc we should do in case the allocator we get
      // from lazy_init is the originating allocator.  (TODO: but note that this
      // can't suddenly become a large deallocation; the only distinction is
      // between being ours to handle and something to post to a Remote.)
      lazy_init(
        [&](CoreAlloc*, CapPtr<void, capptr::bounds::Alloc> p) {
          dealloc(p.unsafe_ptr()); // TODO don't double count statistics
          return nullptr;
        },
        p);
    }

    /**
     * Call `Config::is_initialised()` if it is implemented,
     * unconditionally returns true otherwise.
     */
    SNMALLOC_FAST_PATH
    bool is_initialised()
    {
      return call_is_initialised<Config>(nullptr, 0);
    }

    /**
     * SFINAE helper.  Matched only if `T` implements `ensure_init`.  Calls it
     * if it exists.
     */
    template<typename T>
    SNMALLOC_FAST_PATH auto call_ensure_init(T*, int)
      -> decltype(T::ensure_init())
    {
      T::ensure_init();
    }

    /**
     * SFINAE helper.  Matched only if `T` does not implement `ensure_init`.
     * Does nothing if called.
     */
    template<typename T>
    SNMALLOC_FAST_PATH auto call_ensure_init(T*, long)
    {}

    /**
     * Call `Config::ensure_init()` if it is implemented, do
     * nothing otherwise.
     */
    SNMALLOC_FAST_PATH
    void ensure_init()
    {
      call_ensure_init<Config>(nullptr, 0);
    }

  public:
    constexpr LocalAllocator() = default;
    /**
     * Remove copy constructors and assignment operators.
     * Once initialised the CoreAlloc will take references to the internals
     * of this allocators, and thus copying/moving it is very unsound.
     */
    LocalAllocator(const LocalAllocator&) = delete;
    LocalAllocator& operator=(const LocalAllocator&) = delete;

    /**
     * Initialise the allocator.  For allocators that support local
     * initialisation, this is called with a core allocator that this class
     * allocates (from a pool allocator) the first time it encounters a slow
     * path.  If this class is configured without lazy initialisation support
     * then this must be called externally
     */
    void init(CoreAlloc* c)
    {
      // Initialise the global allocator structures
      ensure_init();

      // Should only be called if the allocator has not been initialised.
      SNMALLOC_ASSERT(core_alloc == nullptr);

      // Attach to it.
      c->attach(&local_cache);
      core_alloc = c;
#ifdef SNMALLOC_TRACING
      message<1024>("init(): core_alloc={} @ {}", core_alloc, &local_cache);
#endif
      // local_cache.stats.sta rt();
    }

    // This is effectively the constructor for the LocalAllocator, but due to
    // not wanting initialisation checks on the fast path, it is initialised
    // lazily.
    void init()
    {
      // Initialise the global allocator structures
      ensure_init();
      // Grab an allocator for this thread.
      init(AllocPool<Config>::acquire());
    }

    // Return all state in the fast allocator and release the underlying
    // core allocator.  This is used during teardown to empty the thread
    // local state.
    void flush()
    {
      // Detached thread local state from allocator.
      if (core_alloc != nullptr)
      {
        core_alloc->flush();

        // core_alloc->stats().add(local_cache.stats);
        // // Reset stats, required to deal with repeated flushing.
        // new (&local_cache.stats) Stats();

        // Detach underlying allocator
        core_alloc->attached_cache = nullptr;
        // Return underlying allocator to the system.
        if constexpr (Config::Options.CoreAllocOwnsLocalState)
        {
          AllocPool<Config>::release(core_alloc);
        }

        // Set up thread local allocator to look like
        // it is new to hit slow paths.
        core_alloc = nullptr;
#ifdef SNMALLOC_TRACING
        message<1024>("flush(): core_alloc={}", core_alloc);
#endif
        local_cache.remote_allocator = &Config::unused_remote;
        local_cache.remote_dealloc_cache.capacity = 0;
      }
    }

    /**
     * Allocate memory of a dynamically known size.
     */
    template<ZeroMem zero_mem = NoZero>
    SNMALLOC_FAST_PATH ALLOCATOR void* alloc(size_t size)
    {
      // Perform the - 1 on size, so that zero wraps around and ends up on
      // slow path.
      if (SNMALLOC_LIKELY(
            (size - 1) <= (sizeclass_to_size(NUM_SMALL_SIZECLASSES - 1) - 1)))
      {
        // Small allocations are more likely. Improve
        // branch prediction by placing this case first.
        return capptr_reveal(small_alloc<zero_mem>(size));
      }

      return capptr_reveal(alloc_not_small<zero_mem>(size));
    }

    // The domestic pointer with its origin allocator
    using DomesticInfo = stl::Pair<capptr::Alloc<void>, const PagemapEntry&>;

    // Check whether the raw pointer is owned by snmalloc
    SNMALLOC_FAST_PATH_INLINE DomesticInfo get_domestic_info(const void* p_raw)
    {
#ifdef __CHERI_PURE_CAPABILITY__
      /*
       * On CHERI platforms, snap the provided pointer to its base, ignoring
       * any client-provided offset, which may have taken the pointer out of
       * bounds and so appear to designate a different object.  The base is
       * is guaranteed by monotonicity either...
       *  * to be within the bounds originally returned by alloc(), or
       *  * one past the end (in which case, the capability length must be 0).
       *
       * Setting the offset does not trap on untagged capabilities, so the tag
       * might be clear after this, as well.
       *
       * For a well-behaved client, this is a no-op: the base is already at the
       * start of the allocation and so the offset is zero.
       */
      p_raw = __builtin_cheri_offset_set(p_raw, 0);
#endif
      capptr::AllocWild<void> p_wild =
        capptr_from_client(const_cast<void*>(p_raw));
      auto p_tame =
        capptr_domesticate<Config>(core_alloc->backend_state_ptr(), p_wild);
      const PagemapEntry& entry =
        Config::Backend::get_metaentry(address_cast(p_tame));
      return {p_tame, entry};
    }

    // Check if a pointer is domestic to SnMalloc
    SNMALLOC_FAST_PATH bool is_snmalloc_owned(const void* p_raw)
    {
      auto [_, entry] = get_domestic_info(p_raw);
      RemoteAllocator* remote = entry.get_remote();
      return remote != nullptr;
    }

    SNMALLOC_FAST_PATH void dealloc(void* p_raw)
    {
      /*
       * p_tame may be nullptr, even if p_raw/p_wild are not, in the case
       * where domestication fails.  We exclusively use p_tame below so that
       * such failures become no ops; in the nullptr path, which should be
       * well off the fast path, we could be slightly more aggressive and test
       * that p_raw is also nullptr and Pal::error() if not. (TODO)
       *
       * We do not rely on the bounds-checking ability of domestication here,
       * and just check the address (and, on other architectures, perhaps
       * well-formedness) of this pointer.  The remainder of the logic will
       * deal with the object's extent.
       */
      auto [p_tame, entry] = get_domestic_info(p_raw);

      if (SNMALLOC_LIKELY(local_cache.remote_allocator == entry.get_remote()))
      {
        dealloc_cheri_checks(p_tame.unsafe_ptr());
        core_alloc->dealloc_local_object(p_tame, entry);
        return;
      }

      dealloc_remote(entry, p_tame);
    }

    SNMALLOC_SLOW_PATH void
    dealloc_remote(const PagemapEntry& entry, capptr::Alloc<void> p_tame)
    {
      RemoteAllocator* remote = entry.get_remote();
      if (SNMALLOC_LIKELY(remote != nullptr))
      {
        dealloc_cheri_checks(p_tame.unsafe_ptr());

        // Detect double free of large allocations here.
        snmalloc_check_client(
          mitigations(sanity_checks),
          !entry.is_backend_owned(),
          "Memory corruption detected");

        // Check if we have space for the remote deallocation
        if (local_cache.remote_dealloc_cache.reserve_space(entry))
        {
          local_cache.remote_dealloc_cache.template dealloc<sizeof(CoreAlloc)>(
            entry.get_slab_metadata(), p_tame, &local_cache.entropy);
#ifdef SNMALLOC_TRACING
          message<1024>(
            "Remote dealloc fast {} ({}, {})",
            address_cast(p_tame),
            alloc_size(p_tame.unsafe_ptr()),
            address_cast(entry.get_slab_metadata()));
#endif
          return;
        }

        dealloc_remote_slow(entry, p_tame);
        return;
      }

      if (SNMALLOC_LIKELY(p_tame == nullptr))
      {
#ifdef SNMALLOC_TRACING
        message<1024>("nullptr deallocation");
#endif
        return;
      }

      dealloc_cheri_checks(p_tame.unsafe_ptr());
      SecondaryAllocator::deallocate(p_tame.unsafe_ptr());
    }

    void check_size(void* p, size_t size)
    {
      if constexpr (mitigations(sanity_checks))
      {
        if (!is_snmalloc_owned(p))
          return;
        size = size == 0 ? 1 : size;
        auto sc = size_to_sizeclass_full(size);
        auto pm_sc =
          Config::Backend::get_metaentry(address_cast(p)).get_sizeclass();
        auto rsize = sizeclass_full_to_size(sc);
        auto pm_size = sizeclass_full_to_size(pm_sc);
        snmalloc_check_client(
          mitigations(sanity_checks),
          (sc == pm_sc) || (p == nullptr),
          "Dealloc rounded size mismatch: {} != {}",
          rsize,
          pm_size);
      }
      else
        UNUSED(p, size);
    }

    SNMALLOC_FAST_PATH void dealloc(void* p, size_t s)
    {
      check_size(p, s);
      dealloc(p);
    }

    template<size_t size>
    SNMALLOC_FAST_PATH void dealloc(void* p)
    {
      check_size(p, size);
      dealloc(p);
    }

    void teardown()
    {
#ifdef SNMALLOC_TRACING
      message<1024>("Teardown: core_alloc={} @ {}", core_alloc, &local_cache);
#endif
      post_teardown = true;
      if (core_alloc != nullptr)
      {
        flush();
      }
    }

    SNMALLOC_FAST_PATH size_t alloc_size(const void* p_raw)
    {
      if (
        !SecondaryAllocator::pass_through && !is_snmalloc_owned(p_raw) &&
        p_raw != nullptr)
        return SecondaryAllocator::alloc_size(p_raw);
      // TODO What's the domestication policy here?  At the moment we just
      // probe the pagemap with the raw address, without checks.  There could
      // be implicit domestication through the `Config::Pagemap` or
      // we could just leave well enough alone.

      // Note that alloc_size should return 0 for nullptr.
      // Other than nullptr, we know the system will be initialised as it must
      // be called with something we have already allocated.
      //
      // To handle this case we require the uninitialised pagemap contain an
      // entry for the first chunk of memory, that states it represents a
      // large object, so we can pull the check for null off the fast path.
      const PagemapEntry& entry =
        Config::Backend::get_metaentry(address_cast(p_raw));

      return sizeclass_full_to_size(entry.get_sizeclass());
    }

    /**
     * Accessor, returns the local cache.  If embedding code is allocating the
     * core allocator for use by this local allocator then it needs to access
     * this field.
     */
    LocalCache<Config>& get_local_cache()
    {
      return local_cache;
    }
  };
} // namespace snmalloc
