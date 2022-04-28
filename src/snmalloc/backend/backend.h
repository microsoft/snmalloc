#pragma once
#include "../backend_helpers/backend_helpers.h"

#if defined(SNMALLOC_CHECK_CLIENT) && !defined(OPEN_ENCLAVE)
/**
 * Protect meta data blocks by allocating separate from chunks for
 * user allocations. This involves leaving gaps in address space.
 * This is less efficient, so should only be applied for the checked
 * build.
 *
 * On Open Enclave the address space is limited, so we disable this
 * feature.
 */
#  define SNMALLOC_META_PROTECTED
#endif

namespace snmalloc
{
  /**
   * This class implements the standard backend for handling allocations.
   * It abstracts page table management and address space management.
   */
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL, bool fixed_range>
  class BackendAllocator : public CommonConfig
  {
  public:
    class PageMapEntry;
    using Pal = PAL;
    using SlabMetadata = FrontendSlabMetadata;

  private:
    using ConcretePagemap =
      FlatPagemap<MIN_CHUNK_BITS, PageMapEntry, PAL, fixed_range>;

  public:
    /**
     * Example of type stored in the pagemap.
     * The following class could be replaced by:
     *
     * ```
     * using PageMapEntry = FrontendMetaEntry<SlabMetadata>;
     * ```
     *
     * The full form here provides an example of how to extend the pagemap
     * entries.  It also guarantees that the front end never directly
     * constructs meta entries, it only ever reads them or modifies them in
     * place.
     */
    class PageMapEntry : public FrontendMetaEntry<SlabMetadata>
    {
      /**
       * The private initialising constructor is usable only by this back end.
       */
      friend class BackendAllocator;

      /**
       * The private default constructor is usable only by the pagemap.
       */
      friend ConcretePagemap;

      /**
       * The only constructor that creates newly initialised meta entries.
       * This is callable only by the back end.  The front end may copy,
       * query, and update these entries, but it may not create them
       * directly.  This contract allows the back end to store any arbitrary
       * metadata in meta entries when they are first constructed.
       */
      SNMALLOC_FAST_PATH
      PageMapEntry(SlabMetadata* meta, uintptr_t ras)
      : FrontendMetaEntry<SlabMetadata>(meta, ras)
      {}

      /**
       * Copy assignment is used only by the pagemap.
       */
      PageMapEntry& operator=(const PageMapEntry& other)
      {
        FrontendMetaEntry<SlabMetadata>::operator=(other);
        return *this;
      }

      /**
       * Default constructor.  This must be callable from the pagemap.
       */
      SNMALLOC_FAST_PATH PageMapEntry() = default;
    };
    using Pagemap = BasicPagemap<
      BackendAllocator,
      PAL,
      ConcretePagemap,
      PageMapEntry,
      fixed_range>;

#if defined(_WIN32) || defined(__CHERI_PURE_CAPABILITY__)
    static constexpr bool CONSOLIDATE_PAL_ALLOCS = false;
#else
    static constexpr bool CONSOLIDATE_PAL_ALLOCS = true;
#endif

    // Set up source of memory
    using P = PalRange<PAL>;
    using Base = std::conditional_t<
      fixed_range,
      EmptyRange,
      PagemapRegisterRange<Pagemap, P, CONSOLIDATE_PAL_ALLOCS>>;

    static constexpr size_t MinBaseSizeBits()
    {
      if constexpr (pal_supports<AlignedAllocation, PAL>)
      {
        return bits::next_pow2_bits_const(PAL::minimum_alloc_size);
      }
      else
      {
        return MIN_CHUNK_BITS;
      }
    }

    // Global range of memory
    using StatsR = StatsRange<
      LargeBuddyRange<Base, 24, bits::BITS - 1, Pagemap, MinBaseSizeBits()>>;
    using GlobalR = GlobalRange<StatsR>;

#ifdef SNMALLOC_META_PROTECTED
    // Source for object allocations
    using ObjectRange =
      LargeBuddyRange<CommitRange<GlobalR, PAL>, 21, 21, Pagemap>;
    // Set up protected range for metadata
    using SubR = CommitRange<SubRange<GlobalR, PAL, 6>, PAL>;
    using MetaRange =
      SmallBuddyRange<LargeBuddyRange<SubR, 21 - 6, bits::BITS - 1, Pagemap>>;
    using GlobalMetaRange = GlobalRange<MetaRange>;
#else
    // Source for object allocations and metadata
    // No separation between the two
    using ObjectRange = SmallBuddyRange<
      LargeBuddyRange<CommitRange<GlobalR, PAL>, 21, 21, Pagemap>>;
    using GlobalMetaRange = GlobalRange<ObjectRange>;
#endif

    struct LocalState
    {
      ObjectRange object_range;

#ifdef SNMALLOC_META_PROTECTED
      MetaRange meta_range;

      MetaRange& get_meta_range()
      {
        return meta_range;
      }
#else
      ObjectRange& get_meta_range()
      {
        return object_range;
      }
#endif
    };

  public:
    template<bool fixed_range_ = fixed_range>
    static std::enable_if_t<!fixed_range_> init()
    {
      static_assert(fixed_range_ == fixed_range, "Don't set SFINAE parameter!");

      Pagemap::concretePagemap.init();
    }

    template<bool fixed_range_ = fixed_range>
    static std::enable_if_t<fixed_range_> init(void* base, size_t length)
    {
      static_assert(fixed_range_ == fixed_range, "Don't set SFINAE parameter!");

      auto [heap_base, heap_length] =
        Pagemap::concretePagemap.init(base, length);

      Pagemap::register_range(address_cast(heap_base), heap_length);

      // Push memory into the global range.
      range_to_pow_2_blocks<MIN_CHUNK_BITS>(
        capptr::Chunk<void>(heap_base),
        heap_length,
        [&](capptr::Chunk<void> p, size_t sz, bool) {
          GlobalR g;
          g.dealloc_range(p, sz);
        });
    }

    /**
     * Provide a block of meta-data with size and align.
     *
     * Backend allocator may use guard pages and separate area of
     * address space to protect this from corruption.
     *
     * The template argument is the type of the metadata being allocated.  This
     * allows the backend to allocate different types of metadata in different
     * places or with different policies.  The default implementation, here,
     * does not avail itself of this degree of freedom.
     */
    template<typename T>
    static capptr::Chunk<void>
    alloc_meta_data(LocalState* local_state, size_t size)
    {
      capptr::Chunk<void> p;
      if (local_state != nullptr)
      {
        p = local_state->get_meta_range().alloc_range_with_leftover(size);
      }
      else
      {
        static_assert(
          GlobalMetaRange::ConcurrencySafe,
          "Global meta data range needs to be concurrency safe.");
        GlobalMetaRange global_state;
        p = global_state.alloc_range(bits::next_pow2(size));
      }

      if (p == nullptr)
        errno = ENOMEM;

      return p;
    }

    /**
     * Returns a chunk of memory with alignment and size of `size`, and a
     * block containing metadata about the slab.
     *
     * It additionally set the meta-data for this chunk of memory to
     * be
     *   (remote, sizeclass, slab_metadata)
     * where slab_metadata, is the second element of the pair return.
     */
    static std::pair<capptr::Chunk<void>, SlabMetadata*>
    alloc_chunk(LocalState& local_state, size_t size, uintptr_t ras)
    {
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);

      auto meta_cap =
        local_state.get_meta_range().alloc_range(sizeof(SlabMetadata));

      auto meta = meta_cap.template as_reinterpret<SlabMetadata>().unsafe_ptr();

      if (meta == nullptr)
      {
        errno = ENOMEM;
        return {nullptr, nullptr};
      }

      auto p = local_state.object_range.alloc_range(size);

#ifdef SNMALLOC_TRACING
      message<1024>("Alloc chunk: {} ({})", p.unsafe_ptr(), size);
#endif
      if (p == nullptr)
      {
        local_state.get_meta_range().dealloc_range(
          meta_cap, sizeof(SlabMetadata));
        errno = ENOMEM;
#ifdef SNMALLOC_TRACING
        message<1024>("Out of memory");
#endif
        return {p, nullptr};
      }

      typename Pagemap::Entry t(meta, ras);
      Pagemap::set_metaentry(address_cast(p), size, t);

      p = Aal::capptr_bound<void, capptr::bounds::Chunk>(p, size);
      return {p, meta};
    }

    static void dealloc_chunk(
      LocalState& local_state,
      SlabMetadata& slab_metadata,
      capptr::Alloc<void> alloc,
      size_t size)
    {
      /*
       * The backend takes possession of these chunks now, by disassociating
       * any existing remote allocator and metadata structure.  If
       * interrogated, the sizeclass reported by the FrontendMetaEntry is 0,
       * which has size 0.
       */
      typename Pagemap::Entry t(nullptr, 0);
      t.claim_for_backend();
      SNMALLOC_ASSERT_MSG(
        Pagemap::get_metaentry(address_cast(alloc)).get_slab_metadata() ==
          &slab_metadata,
        "Slab metadata {} passed for address {} does not match the meta entry "
        "{} that is used for that address",
        &slab_metadata,
        address_cast(alloc),
        Pagemap::get_metaentry(address_cast(alloc)).get_slab_metadata());
      Pagemap::set_metaentry(address_cast(alloc), size, t);

      local_state.get_meta_range().dealloc_range(
        capptr::Chunk<void>(&slab_metadata), sizeof(SlabMetadata));

      // On non-CHERI platforms, we don't need to re-derive to get a pointer to
      // the chunk.  On CHERI platforms this will need to be stored in the
      // SlabMetadata or similar.
      capptr::Chunk<void> chunk{alloc.unsafe_ptr()};
      local_state.object_range.dealloc_range(chunk, size);
    }

    static size_t get_current_usage()
    {
      StatsR stats_state;
      return stats_state.get_current_usage();
    }

    static size_t get_peak_usage()
    {
      StatsR stats_state;
      return stats_state.get_peak_usage();
    }
  };
} // namespace snmalloc
