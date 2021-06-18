#pragma once

#include <pal/pal_noalloc.h>
#include "../backend/backend.h"
#include "../backend/slaballocator.h"
#include "../ds/defines.h"
#include "fastalloc.h"
#include "pool.h"

namespace snmalloc
{
  // Forward reference to thread local cleanup.
  void register_clean_up();

  class CommonConfig
  {
  public:
    using Meta = MetaEntry;
    using Pal = DefaultPal;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static Metaslab default_meta_slab;

    /**
     * Special remote that should never be used as a real remote.
     * This is used to initialise allocators that should always hit the
     * remote path for deallocation. Hence moving a branch of the critical
     * path.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static RemoteAllocator unused_remote;

    /**
     * Special remote that is used in meta-data for large allocations.
     *
     * nullptr is considered a large allocations for this purpose to move
     * of the critical path.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static RemoteAllocator fake_large_remote_impl;

    SNMALLOC_REQUIRE_CONSTINIT
    inline static constexpr RemoteAllocator* fake_large_remote{
      &fake_large_remote_impl};

    /**
     * We use fake_large_remote so that nullptr, will hit the large
     * allocation path which is less performance sensitive.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static MetaEntry default_entry{&default_meta_slab,
                                          fake_large_remote};
  };
}