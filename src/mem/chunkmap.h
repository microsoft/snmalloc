#pragma once

using namespace std;

#include "../ds/address.h"
#include "largealloc.h"
#include "mediumslab.h"
#include "pagemap.h"
#include "slab.h"

namespace snmalloc
{
  enum ChunkMapSuperslabKind
  {
    CMNotOurs = 0,
    CMSuperslab = 1,
    CMMediumslab = 2,

    /*
     * Values 3 (inclusive) through SUPERSLAB_BITS (exclusive) are as yet
     * unused.
     *
     * Values SUPERSLAB_BITS (inclusive) through 64 (exclusive, as it would
     * represent the entire address space) are used for log2(size) at the
     * heads of large allocations.  See SuperslabMap::set_large_size.
     */
    CMLargeMin = SUPERSLAB_BITS,
    CMLargeMax = 63,

    /*
     * Values 64 (inclusive) through 64 + SUPERSLAB_BITS (exclusive) are unused
     */

    /*
     * Values 64 + SUPERSLAB_BITS (inclusive) through 128 (exclusive) are used
     * for entries within a large allocation.  A value of x at pagemap entry p
     * indicates that there are at least 2^(x-64) (inclusive) and at most
     * 2^(x+1-64) (exclusive) page map entries between p and the start of the
     * allocation.  See ChunkMap::set_large_size and external_address's
     * handling of large reallocation redirections.
     */
    CMLargeRangeMin = 64 + SUPERSLAB_BITS,
    CMLargeRangeMax = 127,

    /*
     * Values 128 (inclusive) through 128 + SUPERSLAB_BITS (exclusive) are
     * unused.
     */

    /*
     * Values 128 + SUPERSLAB_BITS (inclusive) through 192 (exclusive) and 192
     * + SUPERSLAB_BITS (inclusive) through 256 (exclusive) are used for
     * foreign regions, paralleling the large region space above.  These values
     * indicate that the memory is managed by a foreign allocator (in another
     * security domain) which we have been told about and, importantly, know
     * how to free back to.  See foreignalloc.h.
     *
     * Finding the allocator in question cannot be done by techniques similar
     * to those used by non-CHERI systems to resolve a CMSuperslab's
     * RemoteAllocator (i.e., pointer arithmetic and reading at a static
     * offset) because the memory may not be trusted.  Instead, there must be
     * some out-of-band store of the RemoteAllocator pointers themselves, akin
     * to CHERI's need for amplification capabilities.  See oobmap.h.
     *
     * Free memory from these regions must be queued per RemoteAllocator and
     * never on the same RemoteList as memory from our security domain (i.e.,
     * ChunkMapSuperslabKinds 1 (inclusive) through 127 (inclusive)), as that
     * would expose memory from our domain to the remote domain, nor on a
     * RemoteList associated with another RemoteAllocator, which could
     * intermingle two different security domains.  (While it is possible that
     * the remote domain has exposed two different RemoteAllocator endpoints to
     * us, and so co-mingling need not breach security boundaries, this seems
     * unlikely to occur in practice, and so we treat all RemoteAllocators as
     * distinct domains.)  The simplest approach is to never queue these in one
     * of our alloctors' RemoteCache structures and always immediately append
     * to the remote in question.
     */

    CMForeignMin = 128 + SUPERSLAB_BITS,
    CMForeignMax = 191,

    /*
     * Values 192 (inclusive) through 192 + SUPERSLAB_BITS (exclusive) are
     * unused.
     */

    CMForeignRangeMin = 192 + SUPERSLAB_BITS,
    CMForeignRangeMax = 255,

  };

  /*
   * Ensure that ChunkMapSuperslabKind values are actually disjoint, i.e.,
   * that large allocations don't land on CMMediumslab.
   */
  static_assert(
    SUPERSLAB_BITS > CMMediumslab, "Large allocations may be too small");

#ifndef SNMALLOC_MAX_FLATPAGEMAP_SIZE
// Use flat map is under a single node.
#  define SNMALLOC_MAX_FLATPAGEMAP_SIZE PAGEMAP_NODE_SIZE
#endif
  static constexpr bool CHUNKMAP_USE_FLATPAGEMAP = pal_supports<LazyCommit> ||
    (SNMALLOC_MAX_FLATPAGEMAP_SIZE >=
     sizeof(FlatPagemap<SUPERSLAB_BITS, uint8_t>));

  using ChunkmapPagemap = std::conditional_t<
    CHUNKMAP_USE_FLATPAGEMAP,
    FlatPagemap<SUPERSLAB_BITS, uint8_t>,
    Pagemap<SUPERSLAB_BITS, uint8_t, 0>>;

  /**
   * Mixin used by `ChunkMap` to directly access the pagemap via a global
   * variable.  This should be used from within the library or program that
   * owns the pagemap.
   *
   * This class makes the global pagemap a static field so that its name
   * includes the type mangling.  If two compilation units try to instantiate
   * two different types of pagemap then they will see two distinct pagemaps.
   * This will prevent allocating with one and freeing with the other (because
   * the memory will show up as not owned by any allocator in the other
   * compilation unit) but will prevent the same memory being interpreted as
   * having two different types.
   */
  template<typename T>
  class GlobalPagemapTemplate
  {
    /**
     * The global pagemap variable.  The name of this symbol will include the
     * type of `T`.
     */
    inline static T global_pagemap;

  public:
    /**
     * Returns the pagemap.
     */
    static ChunkmapPagemap& pagemap()
    {
      return global_pagemap;
    }
  };

  using GlobalPagemap = GlobalPagemapTemplate<ChunkmapPagemap>;

  /**
   * Optionally exported function that accesses the global pagemap provided by
   * a shared library.
   */
  extern "C" void* snmalloc_pagemap_global_get(snmalloc::PagemapConfig const**);

  /**
   * Mixin used by `ChunkMap` to access the global pagemap via a
   * type-checked C interface.  This should be used when another library (e.g.
   * your C standard library) uses snmalloc and you wish to use a different
   * configuration in your program or library, but wish to share a pagemap so
   * that either version can deallocate memory.
   */
  class ExternalGlobalPagemap
  {
    /**
     * A pointer to the pagemap.
     */
    inline static ChunkmapPagemap* external_pagemap;

  public:
    /**
     * Constructor.  Accesses the pagemap via the C ABI accessor and casts it to
     * the expected type, failing in cases of ABI mismatch.
     */
    ExternalGlobalPagemap()
    {
      const snmalloc::PagemapConfig* c;
      external_pagemap =
        ChunkmapPagemap::cast_to_pagemap(snmalloc_pagemap_global_get(&c), c);
      if (!external_pagemap)
      {
        Pal::error("Incorrect ABI of global pagemap.");
      }
    }

    /**
     * Returns the exported pagemap.
     */
    static ChunkmapPagemap& pagemap()
    {
      return *external_pagemap;
    }
  };

  /**
   * Class that defines an interface to the pagemap.  This is provided to
   * `Allocator` as a template argument and so can be replaced by a compatible
   * implementation (for example, to move pagemap updates to a different
   * protection domain).
   */
  template<typename PagemapProvider = GlobalPagemap>
  struct DefaultChunkMap
  {
    /**
     * Get the pagemap entry corresponding to a specific address.
     *
     * Despite the type, the return value is an enum ChunkMapSuperslabKind
     * or one of the reserved values described therewith.
     */
    static uint8_t get(address_t p)
    {
      return PagemapProvider::pagemap().get(p);
    }

    /**
     * Get the pagemap entry corresponding to a specific address.
     */
    static uint8_t get(void* p)
    {
      return get(address_cast(p));
    }

    /**
     * Set a pagemap entry indicating that there is a superslab at the
     * specified index.
     */
    static void set_slab(Superslab* slab)
    {
      set(slab, static_cast<size_t>(CMSuperslab));
    }
    /**
     * Add a pagemap entry indicating that a medium slab has been allocated.
     */
    static void set_slab(Mediumslab* slab)
    {
      set(slab, static_cast<size_t>(CMMediumslab));
    }
    /**
     * Remove an entry from the pagemap corresponding to a superslab.
     */
    static void clear_slab(Superslab* slab)
    {
      SNMALLOC_ASSERT(get(slab) == CMSuperslab);
      set(slab, static_cast<size_t>(CMNotOurs));
    }
    /**
     * Remove an entry corresponding to a medium slab.
     */
    static void clear_slab(Mediumslab* slab)
    {
      SNMALLOC_ASSERT(get(slab) == CMMediumslab);
      set(slab, static_cast<size_t>(CMNotOurs));
    }
    /**
     * Update the pagemap to reflect a large allocation, of `size` bytes from
     * address `p`.
     */
    static void set_large_size(void* p, size_t size)
    {
      size_t size_bits = bits::next_pow2_bits(size);
      set(p, static_cast<uint8_t>(size_bits));
      // Set redirect slide
      auto ss = address_cast(p) + SUPERSLAB_SIZE;
      for (size_t i = 0; i < size_bits - SUPERSLAB_BITS; i++)
      {
        size_t run = 1ULL << i;
        PagemapProvider::pagemap().set_range(
          ss, static_cast<uint8_t>(CMLargeRangeMin + i), run);
        ss = ss + SUPERSLAB_SIZE * run;
      }
    }
    /**
     * Update the pagemap to remove a large allocation, of `size` bytes from
     * address `p`.
     */
    static void clear_large_size(void* vp, size_t size)
    {
      auto p = address_cast(vp);
      size_t rounded_size = bits::next_pow2(size);
      SNMALLOC_ASSERT(get(p) == bits::next_pow2_bits(size));
      auto count = rounded_size >> SUPERSLAB_BITS;
      PagemapProvider::pagemap().set_range(p, CMNotOurs, count);
    }

    /**
     * Set a single Superslab-sized region to being considered foreign.  The
     * OOBMap will tell us what to do with it.
     */
    static void set_foreign(void* p)
    {
      set(p, CMForeignMin);
    }

    /**
     * Set a range of addresses to foreign.
     */
    static void set_foreign_range(void* p, size_t size)
    {
      size_t size_bits = bits::next_pow2_bits(size);
      set(p, CMForeignMin + static_cast<uint8_t>(size_bits));
      // Set redirect slide
      auto ss = address_cast(p) + SUPERSLAB_SIZE;
      for (size_t i = 0; i < size_bits - SUPERSLAB_BITS; i++)
      {
        size_t run = 1ULL << i;
        PagemapProvider::pagemap().set_range(
          ss, static_cast<uint8_t>(CMForeignRangeMin + i), run);
        ss = ss + SUPERSLAB_SIZE * run;
      }
    }

  private:
    /**
     * Helper function to set a pagemap entry.  This is not part of the public
     * interface and exists to make it easy to reuse the code in the public
     * methods in other pagemap adaptors.
     */
    static void set(void* p, uint8_t x)
    {
      PagemapProvider::pagemap().set(address_cast(p), x);
    }
  };

#ifndef SNMALLOC_DEFAULT_CHUNKMAP
#  define SNMALLOC_DEFAULT_CHUNKMAP snmalloc::DefaultChunkMap<>
#endif

} // namespace snmalloc
