#pragma once

#include "snmalloc/aal/address.h"
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

#ifdef SNMALLOC_PASS_THROUGH
#  include "external_alloc.h"
#endif

#include "snmalloc/stl/utility.h"

#include <string.h>

namespace snmalloc
{
  enum Boundary
  {
    /**
     * The location of the first byte of this allocation.
     */
    Start,
    /**
     * The location of the last byte of the allocation.
     */
    End,
    /**
     * The location one past the end of the allocation.  This is mostly useful
     * for bounds checking, where anything less than this value is safe.
     */
    OnePastEnd
  };

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
     * Send all remote deallocation to other threads.
     */
    void post_remote_cache()
    {
      core_alloc->post();
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
        post_remote_cache();
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
     * Abstracts access to the message queue to handle different
     * layout configurations of the allocator.
     */
    auto& message_queue()
    {
      return local_cache.remote_allocator;
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
#ifdef SNMALLOC_PASS_THROUGH
      // snmalloc guarantees a lot of alignment, so we can depend on this
      // make pass through call aligned_alloc with the alignment snmalloc
      // would guarantee.
      void* result = external_alloc::aligned_alloc(
        natural_alignment(size), round_size(size));
      if (zero_mem == YesZero && result != nullptr)
        memset(result, 0, size);
      return result;
#else
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
#endif
    }

    /**
     * Allocate memory of a statically known size.
     */
    template<size_t size, ZeroMem zero_mem = NoZero>
    SNMALLOC_FAST_PATH ALLOCATOR void* alloc()
    {
      return alloc<zero_mem>(size);
    }

    /*
     * Many of these tests come with an "or is null" branch that they'd need to
     * add if we did them up front.  Instead, defer them until we're past the
     * point where we know, from the pagemap, or by explicitly testing, that the
     * pointer under test is not nullptr.
     */
    SNMALLOC_FAST_PATH void dealloc_cheri_checks(void* p)
    {
#if defined(__CHERI_PURE_CAPABILITY__)
      /*
       * Enforce the use of an unsealed capability.
       *
       * TODO In CHERI+MTE, this, is part of the CAmoCDecVersion instruction;
       * elide this test in that world.
       */
      snmalloc_check_client(
        mitigations(cheri_checks),
        !__builtin_cheri_sealed_get(p),
        "Sealed capability in deallocation");

      /*
       * Enforce permissions on the returned pointer.  These pointers end up in
       * free queues and will be cycled out to clients again, so try to catch
       * erroneous behavior now, rather than later.
       *
       * TODO In the CHERI+MTE case, we must reconstruct the pointer for the
       * free queues as part of the discovery of the start of the object (so
       * that it has the correct version), and the CAmoCDecVersion call imposes
       * its own requirements on the permissions (to ensure that it's at least
       * not zero).  They are somewhat more lax than we might wish, so this test
       * may remain, guarded by SNMALLOC_CHECK_CLIENT, but no explicit
       * permissions checks are required in the non-SNMALLOC_CHECK_CLIENT case
       * to defend ourselves or other clients against a misbehaving client.
       */
      static const size_t reqperm = CHERI_PERM_LOAD | CHERI_PERM_STORE |
        CHERI_PERM_LOAD_CAP | CHERI_PERM_STORE_CAP;
      snmalloc_check_client(
        mitigations(cheri_checks),
        (__builtin_cheri_perms_get(p) & reqperm) == reqperm,
        "Insufficient permissions on capability in deallocation");

      /*
       * We check for a valid tag here, rather than in domestication, because
       * domestication might be answering a slightly different question, about
       * the plausibility of addresses rather than of exact pointers.
       *
       * TODO Further, in the CHERI+MTE case, the tag check will be implicit in
       * a future CAmoCDecVersion instruction, and there should be no harm in
       * the lookups we perform along the way to get there.  In that world,
       * elide this test.
       */
      snmalloc_check_client(
        mitigations(cheri_checks),
        __builtin_cheri_tag_get(p),
        "Untagged capability in deallocation");

      /*
       * Verify that the capability is not zero-length, ruling out the other
       * edge case around monotonicity.
       */
      snmalloc_check_client(
        mitigations(cheri_checks),
        __builtin_cheri_length_get(p) > 0,
        "Zero-length capability in deallocation");

      /*
       * At present we check for the pointer also being the start of an
       * allocation closer to dealloc; for small objects, that happens in
       * dealloc_local_object_fast, either below or *on the far end of message
       * receipt*.  For large objects, it happens below by directly rounding to
       * power of two rather than using the is_start_of_object helper.
       * (XXX This does mean that we might end up threading our remote queue
       * state somewhere slightly unexpected rather than at the head of an
       * object.  That is perhaps fine for now?)
       */

      /*
       * TODO
       *
       * We could enforce other policies here, including that the length exactly
       * match the sizeclass.  At present, we bound caps we give for allocations
       * to the underlying sizeclass, so even malloc(0) will have a non-zero
       * length.  Monotonicity would then imply that the pointer must be the
       * head of an object (modulo, perhaps, temporal aliasing if we somehow
       * introduced phase shifts in heap layout like some allocators do).
       *
       * If we switched to bounding with upwards-rounded representable bounds
       * (c.f., CRRL) rather than underlying object size, then we should,
       * instead, in general require plausibility of p_raw by checking that its
       * length is nonzero and the snmalloc size class associated with its
       * length is the one for the slab in question... except for the added
       * challenge of malloc(0).  Since 0 rounds up to 0, we might end up
       * constructing zero-length caps to hand out, which we would then reject
       * upon receipt.  Instead, as part of introducing CRRL bounds, we should
       * introduce a sizeclass for slabs holding zero-size objects.  All told,
       * we would want to check that
       *
       *   size_to_sizeclass(length) == entry.get_sizeclass()
       *
       * I believe a relaxed CRRL test of
       *
       *   length > 0 || (length == sizeclass_to_size(entry.get_sizeclass()))
       *
       * would also suffice and may be slightly less expensive than the test
       * above, at the cost of not catching as many misbehaving clients.
       *
       * In either case, having bounded by CRRL bounds, we would need to be
       * *reconstructing* the capabilities headed to our free lists to be given
       * out to clients again; there are many more CRRL classes than snmalloc
       * sizeclasses (this is the same reason that we can always get away with
       * CSetBoundsExact in capptr_bound).  Switching to CRRL bounds, if that's
       * ever a thing we want to do, will be easier after we've done the
       * plumbing for CHERI+MTE.
       */

      /*
       * TODO: Unsurprisingly, the CHERI+MTE case once again has something to
       * say here.  In that world, again, we are certain to be reconstructing
       * the capability for the free queue anyway, and so exactly what we wish
       * to enforce, length-wise, of the provided capability, is somewhat more
       * flexible.  Using the provided capability bounds when recoloring memory
       * could be a natural way to enforce that it covers the entire object, at
       * the cost of a more elaborate recovery story (as we risk aborting with a
       * partially recolored object).  On non-SNMALLOC_CHECK_CLIENT builds, it
       * likely makes sense to just enforce that length > 0 (*not* enforced by
       * the CAmoCDecVersion instruction) and say that any authority-bearing
       * interior pointer suffices to free the object.  I believe that to be an
       * acceptable security posture for the allocator and between clients;
       * misbehavior is confined to the misbehaving client.
       */
#else
      UNUSED(p);
#endif
    }

    SNMALLOC_FAST_PATH void dealloc(void* p_raw)
    {
#ifdef SNMALLOC_PASS_THROUGH
      external_alloc::free(p_raw);
#else
      // Care is needed so that dealloc(nullptr) works before init
      //  The backend allocator must ensure that a minimal page map exists
      //  before init, that maps null to a remote_deallocator that will never
      //  be in thread local state.

#  ifdef __CHERI_PURE_CAPABILITY__
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
#  endif

      capptr::AllocWild<void> p_wild = capptr_from_client(p_raw);

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
      capptr::Alloc<void> p_tame =
        capptr_domesticate<Config>(core_alloc->backend_state_ptr(), p_wild);

      const PagemapEntry& entry =
        Config::Backend::get_metaentry(address_cast(p_tame));

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
#  ifdef SNMALLOC_TRACING
          message<1024>(
            "Remote dealloc fast {} ({}, {})",
            address_cast(p_tame),
            alloc_size(p_tame.unsafe_ptr()),
            address_cast(entry.get_slab_metadata()));
#  endif
          return;
        }

        dealloc_remote_slow(entry, p_tame);
        return;
      }

      // If p_tame is not null, then dealloc has been call on something
      // it shouldn't be called on.
      // TODO: Should this be tested even in the !CHECK_CLIENT case?
      snmalloc_check_client(
        mitigations(sanity_checks),
        p_tame == nullptr,
        "Not allocated by snmalloc.");

#  ifdef SNMALLOC_TRACING
      message<1024>("nullptr deallocation");
#  endif
      return;
#endif
    }

    void check_size(void* p, size_t size)
    {
#ifdef SNMALLOC_PASS_THROUGH
      UNUSED(p, size);
#else
      if constexpr (mitigations(sanity_checks))
      {
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
#endif
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
#ifdef SNMALLOC_PASS_THROUGH
      return external_alloc::malloc_usable_size(const_cast<void*>(p_raw));
#else
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
#endif
    }

    /**
     * Returns the Start/End of an object allocated by this allocator
     *
     * It is valid to pass any pointer, if the object was not allocated
     * by this allocator, then it give the start and end as the whole of
     * the potential pointer space.
     */
    template<Boundary location = Start>
    void* external_pointer(void* p)
    {
      /*
       * Note that:
       * * each case uses `pointer_offset`, so that on CHERI, our behaviour is
       *   monotone with respect to the capability `p`.
       *
       * * the returned pointer could be outside the CHERI bounds of `p`, and
       *   thus not something that can be followed.
       *
       * * we don't use capptr_from_client()/capptr_reveal(), to avoid the
       *   syntactic clutter.  By inspection, `p` flows only to address_cast
       *   and pointer_offset, and so there's no risk that we follow or act
       *   to amplify the rights carried by `p`.
       */
      if constexpr (location == Start)
      {
        size_t index = index_in_object(address_cast(p));
        return pointer_offset(p, 0 - index);
      }
      else if constexpr (location == End)
      {
        return pointer_offset(p, remaining_bytes(address_cast(p)) - 1);
      }
      else
      {
        return pointer_offset(p, remaining_bytes(address_cast(p)));
      }
    }

    /**
     * @brief Get the client meta data for the snmalloc allocation covering this
     * pointer.
     */
    typename Config::ClientMeta::DataRef get_client_meta_data(void* p)
    {
      const PagemapEntry& entry =
        Config::Backend::get_metaentry(address_cast(p));

      size_t index = slab_index(entry.get_sizeclass(), address_cast(p));

      auto* meta_slab = entry.get_slab_metadata();

      if (SNMALLOC_UNLIKELY(entry.is_backend_owned()))
      {
        error("Cannot access meta-data for write for freed memory!");
      }

      if (SNMALLOC_UNLIKELY(meta_slab == nullptr))
      {
        error(
          "Cannot access meta-data for non-snmalloc object in writable form!");
      }

      return meta_slab->get_meta_for_object(index);
    }

    /**
     * @brief Get the client meta data for the snmalloc allocation covering this
     * pointer.
     */
    stl::add_const_t<typename Config::ClientMeta::DataRef>
    get_client_meta_data_const(void* p)
    {
      const PagemapEntry& entry =
        Config::Backend::template get_metaentry<true>(address_cast(p));

      size_t index = slab_index(entry.get_sizeclass(), address_cast(p));

      auto* meta_slab = entry.get_slab_metadata();

      if (SNMALLOC_UNLIKELY(
            (meta_slab == nullptr) || (entry.is_backend_owned())))
      {
        static typename Config::ClientMeta::StorageType null_meta_store{};
        return Config::ClientMeta::get(&null_meta_store, 0);
      }

      return meta_slab->get_meta_for_object(index);
    }

    /**
     * Returns the number of remaining bytes in an object.
     *
     * auto p = (char*)malloc(size)
     * remaining_bytes(p + n) == size - n     provided n < size
     */
    size_t remaining_bytes(address_t p)
    {
#ifndef SNMALLOC_PASS_THROUGH
      const PagemapEntry& entry =
        Config::Backend::template get_metaentry<true>(p);

      auto sizeclass = entry.get_sizeclass();
      return snmalloc::remaining_bytes(sizeclass, p);
#else
      constexpr address_t mask = static_cast<address_t>(-1);
      constexpr bool is_signed = mask < 0;
      constexpr address_t sign_bit =
        bits::one_at_bit<address_t>(CHAR_BIT * sizeof(address_t) - 1);
      if constexpr (is_signed)
      {
        return (mask ^ sign_bit) - p;
      }
      else
      {
        return mask - p;
      }
#endif
    }

    bool check_bounds(const void* p, size_t s)
    {
      if (SNMALLOC_LIKELY(Config::is_initialised()))
      {
        return remaining_bytes(address_cast(p)) >= s;
      }
      return true;
    }

    /**
     * Returns the byte offset into an object.
     *
     * auto p = (char*)malloc(size)
     * index_in_object(p + n) == n     provided n < size
     */
    size_t index_in_object(address_t p)
    {
#ifndef SNMALLOC_PASS_THROUGH
      const PagemapEntry& entry =
        Config::Backend::template get_metaentry<true>(p);

      auto sizeclass = entry.get_sizeclass();
      return snmalloc::index_in_object(sizeclass, p);
#else
      return reinterpret_cast<size_t>(p);
#endif
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
