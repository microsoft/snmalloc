#pragma once

#include "../ds/ds.h"
#include "../mem/mem.h"
#include "snmalloc/stl/atomic.h"
#include "snmalloc/stl/utility.h"

namespace snmalloc
{
  /**
   * This is a generic implementation of the backend's interface to the page
   * map. It takes a concrete page map implementation (probably FlatPagemap
   * above) and entry type. It is friends with the backend passed in as a
   * template parameter so that the backend can initialise the concrete page map
   * and use set_metaentry which no one else should use.
   */
  template<
    typename PAL,
    typename ConcreteMap,
    typename PagemapEntry,
    bool fixed_range>
  class BasicPagemap
  {
  public:
    /**
     * Export the type stored in the pagemap.
     */
    using Entry = PagemapEntry;

    static_assert(
      stl::is_same_v<PagemapEntry, typename ConcreteMap::EntryType>,
      "BasicPagemap's PagemapEntry and ConcreteMap disagree!");

    static_assert(
      stl::is_base_of_v<MetaEntryBase, PagemapEntry>,
      "BasicPagemap's PagemapEntry type is not a MetaEntryBase");

    /**
     * Prevent snmalloc's backend ranges from consolidating across adjacent OS
     * allocations on platforms (e.g., Windows or StrictProvenance) where
     * that's required.
     */
#if defined(_WIN32) || defined(__CHERI_PURE_CAPABILITY__)
    static constexpr bool CONSOLIDATE_PAL_ALLOCS = false;
#else
    static constexpr bool CONSOLIDATE_PAL_ALLOCS = true;
#endif

    /**
     * Instance of the concrete pagemap, accessible to the backend so that
     * it can call the init method whose type dependent on fixed_range.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    static inline ConcreteMap concretePagemap;

    /**
     * Set the metadata associated with a chunk.
     */
    SNMALLOC_FAST_PATH
    static void set_metaentry(address_t p, size_t size, const Entry& t)
    {
      for (address_t a = p; a < p + size; a += MIN_CHUNK_SIZE)
      {
        concretePagemap.set(a, t);
      }
    }

    /**
     * Set the metadata associated with a chunk, computing per-chunk
     * offset bits for non-pow2 large allocations in the same pass.
     *
     * nat_align is the natural alignment of the size class (slab_mask + 1).
     * Each entry's offset is its distance from the allocation start,
     * measured in nat_align units.
     */
    SNMALLOC_FAST_PATH
    static void set_metaentry(
      address_t p, size_t size, const Entry& t, size_t nat_align)
    {
      size_t chunks_per_nat = nat_align / MIN_CHUNK_SIZE;
      size_t n_chunks = size / MIN_CHUNK_SIZE;
      SNMALLOC_ASSERT_MSG(
        (n_chunks / chunks_per_nat) <= 7,
        "Offset {} exceeds 3-bit field for size {} nat_align {}",
        n_chunks / chunks_per_nat,
        size,
        nat_align);
      size_t chunk_index = 0;
      for (address_t a = p; a < p + size; a += MIN_CHUNK_SIZE)
      {
        concretePagemap.template get_mut<false>(a).assign_with_offset(
          t, static_cast<uint8_t>(chunk_index / chunks_per_nat));
        chunk_index++;
      }
    }

    /**
     * Get the metadata associated with a chunk.
     *
     * Set template parameter to true if it not an error
     * to access a location that is not backed by a chunk.
     */
    template<bool potentially_out_of_range = false>
    SNMALLOC_FAST_PATH static const auto& get_metaentry(address_t p)
    {
      return concretePagemap.template get<potentially_out_of_range>(p);
    }

    /**
     * Get the metadata associated with a chunk.
     *
     * Set template parameter to true if it not an error
     * to access a location that is not backed by a chunk.
     */
    template<bool potentially_out_of_range = false>
    SNMALLOC_FAST_PATH static auto& get_metaentry_mut(address_t p)
    {
      return concretePagemap.template get_mut<potentially_out_of_range>(p);
    }

    /**
     * Register a range in the pagemap as in-use, requiring it to allow writing
     * to the underlying memory.
     *
     * Mark the MetaEntry at the bottom of the range as a boundary, preventing
     * consolidation with a lower range, unless CONSOLIDATE_PAL_ALLOCS.
     */
    static bool register_range(capptr::Arena<void> p, size_t sz)
    {
      auto result = concretePagemap.register_range(address_cast(p), sz);

      if (!result)
      {
        return false;
      }

      if constexpr (!CONSOLIDATE_PAL_ALLOCS)
      {
        // Mark start of allocation in pagemap.
        auto& entry = get_metaentry_mut(address_cast(p));
        entry.set_boundary();
      }
      return true;
    }

    /**
     * Return the bounds of the memory this back-end manages as a pair of
     * addresses (start then end).  This is available iff this is a
     * fixed-range Backend.
     */
    template<bool fixed_range_ = fixed_range>
    static SNMALLOC_FAST_PATH
      stl::enable_if_t<fixed_range_, stl::Pair<address_t, address_t>>
      get_bounds()
    {
      static_assert(fixed_range_ == fixed_range, "Don't set SFINAE parameter!");

      return concretePagemap.get_bounds();
    }

    /**
     * Return whether the pagemap is initialised, ready for access.
     */
    static bool is_initialised()
    {
      return concretePagemap.is_initialised();
    }
  };
} // namespace snmalloc
