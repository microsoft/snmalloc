#pragma once
// If you define SNMALLOC_PROVIDE_OWN_CONFIG then you must provide your own
// definition of `snmalloc::Alloc` before including any files that include
// `snmalloc.h` or consume the global allocation APIs.
#ifndef SNMALLOC_PROVIDE_OWN_CONFIG

#  include "../backend/backend.h"
#  include "meta_protected_range.h"
#  include "standard_range.h"

#  if defined(SNMALLOC_CHECK_CLIENT) && !defined(OPEN_ENCLAVE)
/**
 * Protect meta data blocks by allocating separate from chunks for
 * user allocations. This involves leaving gaps in address space.
 * This is less efficient, so should only be applied for the checked
 * build.
 *
 * On Open Enclave the address space is limited, so we disable this
 * feature.
 */
#    define SNMALLOC_META_PROTECTED
#  endif

namespace snmalloc
{
  // Forward reference to thread local cleanup.
  void register_clean_up();

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

#  if defined(_WIN32) || defined(__CHERI_PURE_CAPABILITY__)
    static constexpr bool CONSOLIDATE_PAL_ALLOCS = false;
#  else
    static constexpr bool CONSOLIDATE_PAL_ALLOCS = true;
#  endif

    using Base = Pipe<
      PalRange<Pal>,
      PagemapRegisterRange<Pagemap, CONSOLIDATE_PAL_ALLOCS>>;

  public:
#  ifdef SNMALLOC_META_PROTECTED
    using LocalState = MetaProtectedRangeLocalState<Pal, Pagemap, Base>;
#  else
    using LocalState = StandardLocalState<Pal, Pagemap, Base>;
#  endif

    using Backend = BackendAllocator<Pal, PageMapEntry, Pagemap, LocalState>;
    using Pal = snmalloc::Pal;
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
