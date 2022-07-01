#pragma once
#include "../pal/pal.h"

namespace snmalloc
{
  /**
   * In CHERI, we must retain, internal to the allocator, the authority to
   * entire backing arenas, as there is no architectural mechanism to splice
   * together two capabilities.  Additionally, these capabilities will retain
   * the VMAP software permission, conveying our authority to manipulate the
   * address space mappings for said arenas.
   *
   * We stash these pointers inside the SlabMetadata structures for parts of
   * the address space for which SlabMetadata exists.  (In other parts of the
   * system, we will stash them directly in the pagemap.)  This requires that
   * we inherit from the FrontendSlabMetadata.
   */
  template<typename SlabMetadata>
  class StrictProvenanceSlabMetadataMixin : public SlabMetadata
  {
    template<SNMALLOC_CONCEPT(IsPAL) A1, typename A2, typename A3, typename A4>
    friend class BackendAllocator;

    capptr::Arena<void> arena;

    /* Set the arena pointer */
    void arena_set(capptr::Arena<void> a)
    {
      arena = a;
    }

    /*
     * Retrieve the stashed pointer for a chunk; the caller must ensure that
     * this is the correct arena for the indicated chunk.  The latter is unused
     * except in debug builds, as there is no architectural amplification.
     */
    capptr::Arena<void> arena_get(capptr::Alloc<void> c)
    {
      SNMALLOC_ASSERT(address_cast(arena) == address_cast(c));
      UNUSED(c);
      return arena;
    }
  };

  /**
   * A dummy implementation of StrictProvenanceBackendSlabMetadata that has no
   * computational content, for use on non-StrictProvenance architectures.
   */
  template<typename SlabMetadata>
  struct LaxProvenanceSlabMetadataMixin : public SlabMetadata
  {
    /* On non-StrictProvenance architectures, there's nothing to do */
    void arena_set(capptr::Arena<void>) {}

    /* Just a type sleight of hand, "amplifying" the non-existant bounds */
    capptr::Arena<void> arena_get(capptr::Alloc<void> c)
    {
      return capptr::Arena<void>::unsafe_from(c.unsafe_ptr());
    }
  };

#ifdef __cpp_concepts
  /**
   * Rather than having the backend test backend_strict_provenance in several
   * places and doing sleights of hand with the type system, we encapsulate
   * the amplification
   */
  template<typename T>
  concept IsSlabMeta_Arena = requires(T* t, capptr::Arena<void> p)
  {
    {
      t->arena_set(p)
    }
    ->ConceptSame<void>;
  }
  &&requires(T* t, capptr::Alloc<void> p)
  {
    {
      t->arena_get(p)
    }
    ->ConceptSame<capptr::Arena<void>>;
  };
#endif

} // namespace snmalloc
