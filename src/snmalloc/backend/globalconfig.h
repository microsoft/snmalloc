#pragma once
// If you define SNMALLOC_PROVIDE_OWN_CONFIG then you must provide your own
// definition of `snmalloc::Alloc` before including any files that include
// `snmalloc.h` or consume the global allocation APIs.
#ifndef SNMALLOC_PROVIDE_OWN_CONFIG

#  include "../backend/backend.h"
#include "strictprovenancebackend.h"

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

  using BackendAlloc = std::conditional_t<
    aal_supports<StrictProvenance, Aal>,
    StrictProvenanceBackend<Pal>,
    BackendAllocator<Pal, false>>;

  /**
   * The default configuration for a global snmalloc.  This allocates memory
   * from the operating system and expects to manage memory anywhere in the
   * address space.
   */
  class Globals final : public BackendAlloc
  {
  public:
    using GlobalPoolState = PoolState<CoreAllocator<Globals>>;

  private:
    using Backend = BackendAlloc;

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
      Backend::init();

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
