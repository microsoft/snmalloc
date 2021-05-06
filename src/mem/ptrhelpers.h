#pragma once

#include "../aal/aal.h"
#include "../ds/ptrwrap.h"
#include "allocconfig.h"

namespace snmalloc
{
  /*
   * At various points, we do pointer math on high-authority pointers to find
   * some metadata.  `capptr_bound_chunkd` and `capptr_chunk_from_chunkd`
   * encapsulate the notion that the result of these accesses is left unbounded
   * in non-debug builds, because most codepaths do not reveal these pointers or
   * any progeny to the application.  However, in some cases we have already
   * (partially) bounded these high-authority pointers (to CBChunk) and wish to
   * preserve this annotation (rather than always returning a CBChunkD-annotated
   * pointer); `capptr_bound_chunkd_bounds` does the computation for us and is
   * used in the signatures of below and in those of wrappers around them.
   */

  template<capptr_bounds B>
  constexpr capptr_bounds capptr_bound_chunkd_bounds()
  {
    switch (B)
    {
      case CBArena:
        return CBChunkD;
      case CBChunkD:
        return CBChunkD;
      case CBChunk:
        return CBChunk;
    }
    SNMALLOC_UNREACHABLE;
  }

  /**
   * Construct an CapPtr<T, CBChunkD> from an CapPtr<T, CBArena> or
   * CapPtr<T, CBChunkD> input.  For an CapPtr<T, CBChunk> input, simply pass
   * it through (preserving the static notion of bounds).
   *
   * Applies bounds on debug builds, otherwise is just sleight of hand.
   *
   * Requires that `p` point at a multiple of `sz` (that is, at the base of a
   * highly-aligned object) to avoid representability issues.
   */
  template<typename T, capptr_bounds B>
  SNMALLOC_FAST_PATH CapPtr<T, capptr_bound_chunkd_bounds<B>()>
  capptr_bound_chunkd(CapPtr<T, B> p, size_t sz)
  {
    static_assert(B == CBArena || B == CBChunkD || B == CBChunk);
    SNMALLOC_ASSERT((address_cast(p) % sz) == 0);

#ifndef NDEBUG
    // On Debug builds, apply bounds if not already there
    if constexpr (B == CBArena)
      return Aal::capptr_bound<T, CBChunkD>(p, sz);
    else // quiesce MSVC's warnings about unreachable code below
#endif
    {
      UNUSED(sz);
      return CapPtr<T, capptr_bound_chunkd_bounds<B>()>(p.unsafe_capptr);
    }
    SNMALLOC_UNREACHABLE;
  }

  /**
   * Apply bounds that might not have been applied when constructing an
   * CapPtr<T, CBChunkD>.  That is, on non-debug builds, apply bounds; debug
   * builds have already had them applied.
   *
   * Requires that `p` point at a multiple of `sz` (that is, at the base of a
   * highly-aligned object) to avoid representability issues.
   */
  template<typename T>
  SNMALLOC_FAST_PATH CapPtr<T, CBChunk>
  capptr_chunk_from_chunkd(CapPtr<T, CBChunkD> p, size_t sz)
  {
    SNMALLOC_ASSERT((address_cast(p) % sz) == 0);

#ifndef NDEBUG
    // On debug builds, CBChunkD are already bounded as if CBChunk.
    UNUSED(sz);
    return CapPtr<T, CBChunk>(p.unsafe_capptr);
#else
    // On non-debug builds, apply bounds now, as they haven't been already.
    return Aal::capptr_bound<T, CBChunk>(p, sz);
#endif
  }

  /**
   * Very rarely, while debugging, it's both useful and acceptable to forget
   * that we have applied chunk bounds to something.
   */
  template<typename T>
  SNMALLOC_FAST_PATH CapPtr<T, CBChunkD>
  capptr_debug_chunkd_from_chunk(CapPtr<T, CBChunk> p)
  {
    return CapPtr<T, CBChunkD>(p.unsafe_capptr);
  }
} // namespace snmalloc
