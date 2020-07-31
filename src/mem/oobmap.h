#pragma once

namespace snmalloc
{
  /*
   * Like the ChunkMap, the OOBMap might be flat or might be paged.
   *
   * Unlike the ChunkMap, which stored a byte per granule, the OOBMap
   * stores a pointer per granule (which are generally larger than the
   * PageMap's).
   */

  static constexpr bool OOBMAP_USE_FLATPAGEMAP = pal_supports<LazyCommit> ||
    (SNMALLOC_MAX_FLATPAGEMAP_SIZE >=
     sizeof(FlatPagemap<OOBMAP_BITS, uintptr_t>));

  using OOBMapPagemap = std::conditional_t<
    OOBMAP_USE_FLATPAGEMAP,
    FlatPagemap<OOBMAP_BITS, uintptr_t>,
    Pagemap<OOBMAP_BITS, uintptr_t, 0>>;

  static const size_t OOBMAP_SIZE = 1ULL << OOBMAP_BITS;

  /*
   * See GlobalPagemapTemplate.
   */
  template<typename T>
  class GlobalOOBMapTemplate
  {
    inline static T global_oobmap;

  public:
    static OOBMapPagemap& pagemap()
    {
      return global_oobmap;
    }
  };

  using GlobalOOBMap = GlobalOOBMapTemplate<OOBMapPagemap>;

  /*
   * TODO: We probably also want to duplicate the `ExternalGlobalPagemap`
   * machinery for the `OOBMap`, too.
   */

  template<typename OOBMapProvider = GlobalOOBMap>
  struct DefaultOOBMap
  {
    /// Get the metadata for a given address.
    static void* get(address_t p)
    {
      return reinterpret_cast<void*>(OOBMapProvider::pagemap().get(p));
    }

    static void* get(void* p)
    {
      return get(address_cast(p));
    }

    /// Set the metadata for a given address
    static void set_oob(address_t p, void* f)
    {
      OOBMapProvider::pagemap().set(p, reinterpret_cast<uintptr_t>(f));
    }

    static void set_oob(void* p, void* f)
    {
      set_oob(address_cast(p), f);
    }

    /// Set the metadata for a range of addresses
    static void set_oob_range(void* p, size_t size, void* f)
    {
      auto pc = address_cast(p);
      auto fc = reinterpret_cast<uintptr_t>(f);

      size_t size_bits = bits::next_pow2_bits(size);
      OOBMapProvider::pagemap().set(pc, fc);
      auto ps = pc + OOBMAP_SIZE;
      for (size_t i = 0; i < size_bits - OOBMAP_BITS; i++)
      {
        size_t run = 1ULL << i;
        OOBMapProvider::pagemap().set_range(ps, fc, run);
        ps += OOBMAP_SIZE * run;
      }
    }
  };

#ifndef SNMALLOC_DEFAULT_OOBMAP
#  define SNMALLOC_DEFAULT_OOBMAP snmalloc::DefaultOOBMap<>
#endif

} // namespace snmalloc
