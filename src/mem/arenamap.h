#include "../ds/ptrwrap.h"
#include "pagemap.h"

namespace snmalloc
{
  struct default_alloc_size_t
  {
    /*
     * Just make something up for non-StrictProvenance architectures.
     * Ultimately, this is going to flow only to FlatPagemap's template argument
     * for the number of bits it's covering but the whole thing will be
     * discarded by the time we resolve all the conditionals behind the
     * AuthPagemap type.  To avoid pathologies where COVERED_BITS ends up being
     * bit-width of the machine (meaning 1ULL << COVERED_BITS becomes undefined)
     * and where sizeof(std::atomic<T>[ENTRIES]) is either undefined or
     * enormous, we choose a value that dodges both endpoints and still results
     * in a small table.
     */
    static constexpr size_t capptr_root_alloc_size =
      bits::one_at_bit(bits::ADDRESS_BITS - 8);
  };

  /*
   * Compute the block allocation size to use for AlignedAllocations.  This
   * is either PAL::capptr_root_alloc_size, on architectures that require
   * StrictProvenance, or the placeholder from above.
   */
  template<typename PAL>
  static constexpr size_t AUTHMAP_ALLOC_SIZE = std::conditional_t<
    aal_supports<StrictProvenance>,
    PAL,
    default_alloc_size_t>::capptr_root_alloc_size;

  template<typename PAL>
  static constexpr size_t
    AUTHMAP_BITS = bits::next_pow2_bits_const(AUTHMAP_ALLOC_SIZE<PAL>);

  template<typename PAL>
  static constexpr bool
    AUTHMAP_USE_FLATPAGEMAP = pal_supports<LazyCommit, PAL> ||
    (PAGEMAP_NODE_SIZE >= sizeof(FlatPagemap<AUTHMAP_BITS<PAL>, void*>));

  struct default_auth_pagemap
  {
    static SNMALLOC_FAST_PATH void* get(address_t a)
    {
      UNUSED(a);
      return nullptr;
    }
  };

  template<typename PAL, typename PrimAlloc>
  using AuthPagemap = std::conditional_t<
    aal_supports<StrictProvenance>,
    std::conditional_t<
      AUTHMAP_USE_FLATPAGEMAP<PAL>,
      FlatPagemap<AUTHMAP_BITS<PAL>, void*>,
      Pagemap<AUTHMAP_BITS<PAL>, void*, nullptr, PrimAlloc>>,
    default_auth_pagemap>;

  struct ForAuthmap
  {};
  template<typename PAL, typename PrimAlloc>
  using GlobalAuthmap =
    GlobalPagemapTemplate<AuthPagemap<PAL, PrimAlloc>, ForAuthmap>;

  template<SNMALLOC_CONCEPT(ConceptPAL) PAL, typename PagemapProvider>
  struct DefaultArenaMapTemplate
  {
    /*
     * Without AlignedAllocation, we (below) adopt a fallback mechanism that
     * over-allocates and then finds an aligned region within the too-large
     * region.  The "trimmings" from either side are also registered in hopes
     * that they can be used for later allocations.
     *
     * Unfortunately, that strategy does not work for this ArenaMap: trimmings
     * may be smaller than the granularity of our backing PageMap, and so we
     * would be unable to amplify authority.  Eventually we may arrive at a need
     * for an ArenaMap that is compatible with this approach, but for the moment
     * it's far simpler to assume that we can always ask for memory sufficiently
     * aligned to cover an entire PageMap granule.
     */
    static_assert(
      !aal_supports<StrictProvenance> || pal_supports<AlignedAllocation, PAL>,
      "StrictProvenance requires platform support for aligned allocation");

    static constexpr size_t alloc_size = AUTHMAP_ALLOC_SIZE<PAL>;

    /*
     * Because we assume that we can `capptr_amplify` and then
     * `Superslab::get()` on the result to get to the Superslab metadata
     * headers, it must be the case that provenance roots cover entire
     * Superslabs.
     */
    static_assert(
      !aal_supports<StrictProvenance> ||
        ((alloc_size > 0) && (alloc_size % SUPERSLAB_SIZE == 0)),
      "Provenance root granule must encompass whole superslabs");

    static void register_root(CapPtr<void, CBArena> root)
    {
      if constexpr (aal_supports<StrictProvenance>)
      {
        PagemapProvider::pagemap().set(address_cast(root), root.unsafe_capptr);
      }
      else
      {
        UNUSED(root);
      }
    }

    template<
      typename T = void,
      typename U,
      SNMALLOC_CONCEPT(capptr_bounds::c) B>
    static SNMALLOC_FAST_PATH CapPtr<T, CBArena> capptr_amplify(CapPtr<U, B> r)
    {
      static_assert(
        B::spatial == capptr_bounds::spatial::Alloc,
        "Attempting to amplify an unexpectedly high pointer");

      return Aal::capptr_rebound(
               CapPtr<void, CBArena>(
                 PagemapProvider::pagemap().get(address_cast(r))),
               r)
        .template as_static<T>();
    }
  };

  template<typename PAL, typename PrimAlloc>
  using DefaultArenaMap =
    DefaultArenaMapTemplate<PAL, GlobalAuthmap<PAL, PrimAlloc>>;

} // namespace snmalloc
