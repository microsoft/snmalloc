#pragma once
// If you define SNMALLOC_PROVIDE_OWN_CONFIG then you must provide your own
// definition of `snmalloc::Alloc` before including any files that include
// `snmalloc.h` or consume the global allocation APIs.
#ifndef SNMALLOC_PROVIDE_OWN_CONFIG

#  include "../backend/backend.h"

namespace snmalloc
{
  // Forward reference to thread local cleanup.
  void register_clean_up();

#  ifdef USE_SNMALLOC_STATS
  inline static void print_stats()
  {
    printf("No Stats yet!");
    // Stats s;
    // current_alloc_pool()->aggregate_stats(s);
    // s.print<Alloc>(std::cout);
  }
#  endif

  /**
   * The default configuration for a global snmalloc.  This allocates memory
   * from the operating system and expects to manage memory anywhere in the
   * address space.
   */
  class Globals final : public CommonConfig
  {
  public:
    using GlobalPoolState = PoolState<CoreAllocator<Globals>>;

  private:
    using ConcretePagemap =
      FlatPagemap<MIN_CHUNK_BITS, PageMapEntry, Pal, false>;

    using Pagemap = BasicPagemap<Pal, ConcretePagemap, PageMapEntry, false>;

  public:
#  if defined(_WIN32) || defined(__CHERI_PURE_CAPABILITY__)
    static constexpr bool CONSOLIDATE_PAL_ALLOCS = false;
#  else
    static constexpr bool CONSOLIDATE_PAL_ALLOCS = true;
#  endif

    // Set up source of memory
    using Base = Pipe<
      PalRange<Pal>,
      PagemapRegisterRange<Pagemap, CONSOLIDATE_PAL_ALLOCS>>;

    // Global range of memory
    using GlobalR = Pipe<
      Base,
      LargeBuddyRange<24, bits::BITS - 1, Pagemap, MinBaseSizeBits<Pal>()>,
      LogRange<2>,
      GlobalRange<>>;

#  ifdef SNMALLOC_META_PROTECTED
    // Introduce two global ranges, so we don't mix Object and Meta
    using CentralObjectRange = Pipe<
      GlobalR,
      LargeBuddyRange<24, bits::BITS - 1, Pagemap>,
      LogRange<3>,
      GlobalRange<>>;

    using CentralMetaRange = Pipe<
      GlobalR,
      SubRange<Pal, 6>, // Use SubRange to introduce guard pages.
      LargeBuddyRange<24, bits::BITS - 1, Pagemap>,
      LogRange<4>,
      GlobalRange<>>;

    // Source for object allocations
    using StatsObject =
      Pipe<CentralObjectRange, CommitRange<Pal>, StatsRange<>>;

    using ObjectRange =
      Pipe<StatsObject, LargeBuddyRange<21, 21, Pagemap>, LogRange<5>>;

    using StatsMeta = Pipe<CentralMetaRange, CommitRange<Pal>, StatsRange<>>;

    using MetaRange = Pipe<
      StatsMeta,
      LargeBuddyRange<21 - 6, bits::BITS - 1, Pagemap>,
      SmallBuddyRange<>>;

    // Create global range that can service small meta-data requests.
    // Don't want to add this to the CentralMetaRange to move Commit outside the
    // lock on the common case.
    using GlobalMetaRange = Pipe<StatsMeta, SmallBuddyRange<>, GlobalRange<>>;
    using Stats = StatsCombiner<StatsObject, StatsMeta>;
#  else
    // Source for object allocations and metadata
    // No separation between the two
    using Stats = Pipe<GlobalR, StatsRange<>>;
    using ObjectRange = Pipe<
      Stats,
      CommitRange<Pal>,
      LargeBuddyRange<21, 21, Pagemap>,
      SmallBuddyRange<>>;
    using GlobalMetaRange = Pipe<ObjectRange, GlobalRange<>>;
#  endif

  public:
    struct LocalState
    {
      ObjectRange object_range;

#  ifdef SNMALLOC_META_PROTECTED
      MetaRange meta_range;

      MetaRange& get_meta_range()
      {
        return meta_range;
      }
#  else
      ObjectRange& get_meta_range()
      {
        return object_range;
      }
#  endif

      using Stats = Stats;

      using GlobalMetaRange = GlobalMetaRange;
    };

    using Backend = BackendAllocator<
      Pal,
      PageMapEntry,
      Pagemap,
      LocalState>;
    using Pal = Pal;
    using SlabMetadata = typename Backend::SlabMetadata;

  private:
    SNMALLOC_REQUIRE_CONSTINIT
    inline static GlobalPoolState alloc_pool;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static std::atomic<bool> initialised{false};

    SNMALLOC_REQUIRE_CONSTINIT
    inline static FlagWord initialisation_lock{};

  public:
    static GlobalPoolState& pool()
    {
      return alloc_pool;
    }

    static constexpr Flags Options{};

    // Performs initialisation for this configuration
    // of allocators.  Needs to be idempotent,
    // and concurrency safe.
    static void ensure_init()
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
      key_global = FreeListKey(entropy.get_free_list_key());

      // Need to initialise pagemap.
      Pagemap::concretePagemap.init();

#  ifdef USE_SNMALLOC_STATS
      atexit(snmalloc::print_stats);
#  endif

      initialised = true;
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
} // namespace snmalloc

// The default configuration for snmalloc
namespace snmalloc
{
  using Alloc = snmalloc::LocalAllocator<snmalloc::Globals>;
} // namespace snmalloc
#endif
