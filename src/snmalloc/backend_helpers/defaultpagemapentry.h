#pragma once

#include "../mem/mem.h"

namespace snmalloc
{
  /**
   * Example of type stored in the pagemap.
   * The following class could be replaced by:
   *
   * ```
   * using DefaultPagemapEntry = FrontendMetaEntry<SlabMetadata>;
   * ```
   *
   * The full form here provides an example of how to extend the pagemap
   * entries.  It also guarantees that the front end never directly
   * constructs meta entries, it only ever reads them or modifies them in
   * place.
   */
  class DefaultPagemapEntry : public FrontendMetaEntry<FrontendSlabMetadata>
  {
    /**
     * The private initialising constructor is usable only by this back end.
     */
    template<
      SNMALLOC_CONCEPT(ConceptPAL) A1,
      typename A2,
      typename A3,
      typename A4>
    friend class BackendAllocator;

    /**
     * The private default constructor is usable only by the pagemap.
     */
    template<size_t GRANULARITY_BITS, typename T, typename PAL, bool has_bounds>
    friend class FlatPagemap;

    /**
     * The only constructor that creates newly initialised meta entries.
     * This is callable only by the back end.  The front end may copy,
     * query, and update these entries, but it may not create them
     * directly.  This contract allows the back end to store any arbitrary
     * metadata in meta entries when they are first constructed.
     */
    SNMALLOC_FAST_PATH
    DefaultPagemapEntry(FrontendSlabMetadata* meta, uintptr_t ras)
    : FrontendMetaEntry<FrontendSlabMetadata>(meta, ras)
    {}

    /**
     * Copy assignment is used only by the pagemap.
     */
    DefaultPagemapEntry& operator=(const DefaultPagemapEntry& other)
    {
      FrontendMetaEntry<FrontendSlabMetadata>::operator=(other);
      return *this;
    }

    /**
     * Default constructor.  This must be callable from the pagemap.
     */
    SNMALLOC_FAST_PATH DefaultPagemapEntry() = default;
  };
} // namespace snmalloc