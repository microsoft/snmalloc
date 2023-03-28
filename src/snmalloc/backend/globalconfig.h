#pragma once
// If you define SNMALLOC_PROVIDE_OWN_CONFIG then you must provide your own
// definition of `snmalloc::Alloc` before including any files that include
// `snmalloc.h` or consume the global allocation APIs.
#ifndef SNMALLOC_PROVIDE_OWN_CONFIG

#  include "../backend_helpers/backend_helpers.h"
#  include "backend.h"
#  include "meta_protected_range.h"
#  include "standard_range.h"

namespace snmalloc
{
  // Forward reference to thread local cleanup.
  void register_clean_up();

  /**
   * The default configuration for a global snmalloc.  It contains all the
   * datastructures to manage the memory from the OS.  It had several internal
   * public types for various aspects of the code.
   * The most notable are:
   *
   *   Backend - Manages the memory coming from the platform.
   *   LocalState - the per-thread/per-allocator state that may perform local
   *     caching of reserved memory. This also specifies the various Range types
   *     used to manage the memory.
   *
   * The Configuration sets up a Pagemap for the backend to use, and the state
   * required to build new allocators (GlobalPoolState).
   */
  class StandardConfig final : public CommonConfig
  {
    using GlobalPoolState = PoolState<CoreAllocator<StandardConfig>>;

  public:
    using Pal = DefaultPal;
    using PagemapEntry = DefaultPagemapEntry;

  private:
    using ConcretePagemap =
      FlatPagemap<MIN_CHUNK_BITS, PagemapEntry, Pal, false>;

    using Pagemap = BasicPagemap<Pal, ConcretePagemap, PagemapEntry, false>;

    using ConcreteAuthmap =
      FlatPagemap<MinBaseSizeBits<Pal>(), capptr::Arena<void>, Pal, false>;

    using Authmap = DefaultAuthmap<ConcreteAuthmap>;

    /**
     * This specifies where this configurations sources memory from and the
     * pagemap (and authmap) that maintain metadata about underlying OS
     * allocations.
     * @{
     */

    using Base = Pipe<
      PalRange<Pal>,
      PagemapRegisterRange<Pagemap>,
      PagemapRegisterRange<Authmap>>;

    /**
     * @}
     */
  public:
    /**
     * Use one of the default range configurations
     */
    using LocalState = std::conditional_t<
      mitigations(metadata_protection),
      MetaProtectedRangeLocalState<Pal, Pagemap, Base>,
      StandardLocalState<Pal, Pagemap, Base>>;

    /**
     * Use the default backend.
     */
    using Backend =
      BackendAllocator<Pal, PagemapEntry, Pagemap, Authmap, LocalState>;

  private:
    SNMALLOC_REQUIRE_CONSTINIT
    inline static GlobalPoolState alloc_pool;

    /**
     * Specifies if the Configuration has been initialised.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static std::atomic<bool> initialised{false};

    /**
     * Used to prevent two threads attempting to initialise the configuration
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static FlagWord initialisation_lock{};

    // Performs initialisation for this configuration
    // of allocators.
    SNMALLOC_SLOW_PATH static void ensure_init_slow()
    {
      FlagLock lock{initialisation_lock};
#  ifdef SNMALLOC_TRACING
      message<1024>("Run init_impl");
#  endif

      if (initialised)
        return;

      LocalEntropy entropy;
      entropy.init<Pal>();
      // Initialise key for remote deallocation lists
      RemoteAllocator::key_global = FreeListKey(entropy.get_free_list_key());

      // Need to randomise pagemap location. If requested and not a
      // StrictProvenance architecture, randomize its table's location within a
      // significantly larger address space allocation.
      static constexpr bool pagemap_randomize =
        mitigations(random_pagemap) && !aal_supports<StrictProvenance>;

      Pagemap::concretePagemap.template init<pagemap_randomize>();

      if constexpr (aal_supports<StrictProvenance>)
      {
        Authmap::init();
      }

      initialised.store(true, std::memory_order_release);
    }

  public:
    /**
     * Provides the state to create new allocators.
     */
    static GlobalPoolState& pool()
    {
      return alloc_pool;
    }

    static constexpr Flags Options{};

    // Performs initialisation for this configuration
    // of allocators.  Needs to be idempotent,
    // and concurrency safe.
    SNMALLOC_FAST_PATH static void ensure_init()
    {
      if (SNMALLOC_LIKELY(initialised.load(std::memory_order_acquire)))
        return;

      ensure_init_slow();
    }

    static bool is_initialised()
    {
      return initialised;
    }

    // This needs to be a forward reference as the
    // thread local state will need to know about this.
    // This may allocate, so should only be called once
    // a thread local allocator is available.
    static void register_clean_up()
    {
      snmalloc::register_clean_up();
    }
  };

  /**
   * Create allocator type for this configuration.
   */
  using Alloc = snmalloc::LocalAllocator<snmalloc::StandardConfig>;
} // namespace snmalloc
#endif
