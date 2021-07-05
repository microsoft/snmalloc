#pragma once

#include "../ds/defines.h"
#include "localalloc.h"
#include "pool.h"

#include <pal/pal_noalloc.h>

namespace snmalloc
{
  // Forward reference to thread local cleanup.
  void register_clean_up();

  class CommonConfig
  {
  public:
    using Pal = DefaultPal;

    /**
     * Special remote that should never be used as a real remote.
     * This is used to initialise allocators that should always hit the
     * remote path for deallocation. Hence moving a branch off the critical
     * path.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static RemoteAllocator unused_remote;

    /**
     * Special remote that is used in meta-data for large allocations.
     *
     * nullptr is considered a large allocations for this purpose to move
     * of the critical path.
     *
     * Bottom bits of the remote pointer are used for a sizeclass, we need
     * size bits to represent the non-large sizeclasses, we can then get
     * the large sizeclass by having the fake large_remote considerably
     * more aligned.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static constexpr RemoteAllocator* fake_large_remote{nullptr};

    static_assert(
      &unused_remote != fake_large_remote,
      "Compilation should ensure these are different");
  };
}