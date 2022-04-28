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
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL>
  class StrictProvenanceBackend : public CommonConfig
  {
  public:
    static constexpr Flags Options = []() constexpr
    {
      Flags opts = {};
      opts.HasReversion = true;
      return opts;
    }
    ();

    using Pal = PAL;
    class SlabMetadata : public FrontendSlabMetadata
    {
      friend StrictProvenanceBackend;

    private:
      /**
       * In the strict provenance world we need to retain a pointer to the
       * entire slab (actually bounded to mmapped region) for various uses:
       *  - on deallocation, to clear the slab and allow merging in backend
       *  - on CHERI+MTE as the authority for reversioning
       * This is a high privilege pointer and we should consider protecting it
       * (perhaps by sealing).
       */
      capptr::Chunk<void> chunk_ptr;
    };
    using PageMapEntry = FrontendMetaEntry<SlabMetadata>;

  private:
    using ConcretePagemap =
      FlatPagemap<MIN_CHUNK_BITS, PageMapEntry, PAL, false>;

  public:
    using Pagemap = BasicPagemap<
      StrictProvenanceBackend,
      PAL,
      ConcretePagemap,
      PageMapEntry,
      false>;

    static constexpr bool CONSOLIDATE_PAL_ALLOCS = false;

    // Set up source of memory
    using Base = Pipe<
      PalRange<DefaultPal>,
      PagemapRegisterRange<Pagemap,CONSOLIDATE_PAL_ALLOCS>>;

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
    using GlobalR = Pipe<
      Base,
      LargeBuddyRange<24, bits::BITS - 1, Pagemap, MinBaseSizeBits()>,
      LogRange<2>,
      GlobalRange<>>;

#ifdef SNMALLOC_META_PROTECTED
    // Introduce two global ranges, so we don't mix Object and Meta
    using CentralObjectRange = Pipe<
      GlobalR,
      LargeBuddyRange<24, bits::BITS - 1, Pagemap, MinBaseSizeBits()>,
      LogRange<3>,
      GlobalRange<>>;

    using CentralMetaRange = Pipe<
      GlobalR,
      SubRange<PAL, 6>, // Use SubRange to introduce guard pages.
      LargeBuddyRange<24, bits::BITS - 1, Pagemap, MinBaseSizeBits()>,
      LogRange<4>,
      GlobalRange<>>;

    // Source for object allocations
    using StatsObject =
      Pipe<CentralObjectRange, CommitRange<PAL>, StatsRange<>>;

    using ObjectRange =
      Pipe<StatsObject, LargeBuddyRange<21, 21, Pagemap>, LogRange<5>>;

    using StatsMeta = Pipe<CentralMetaRange, CommitRange<PAL>, StatsRange<>>;

    using MetaRange = Pipe<
      StatsMeta,
      LargeBuddyRange<21 - 6, bits::BITS - 1, Pagemap>,
      SmallBuddyRange<>>;

    // Create global range that can service small meta-data requests.
    // Don't want to add this to the CentralMetaRange to move Commit outside the
    // lock on the common case.
    using GlobalMetaRange = Pipe<StatsMeta, SmallBuddyRange<>, GlobalRange<>>;
    using Stats = StatsCombiner<StatsObject, StatsMeta>;
#else
    // Source for object allocations and metadata
    // No separation between the two
    using Stats = Pipe<GlobalR, StatsRange<>>;
    using ObjectRange = Pipe<
      Stats,
      CommitRange<PAL>,
      LargeBuddyRange<21, 21, Pagemap>,
      SmallBuddyRange<>>;
    using GlobalMetaRange = Pipe<ObjectRange, GlobalRange<>>;
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
    static void init()
    {

      Pagemap::concretePagemap.init();
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

      /* Set the chunk pointer in the slab metadata. This is necessary for
      strict provenance on deallocation. */
      meta->chunk_ptr = p;

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
      SNMALLOC_ASSERT(address_cast(slab_metadata.chunk_ptr) == address_cast(alloc));
      Pagemap::set_metaentry(address_cast(alloc), size, t);

      // On CHERI platforms, we free the pointer for the chunk that we
      // earlier stashed in the slab_metadata, otherwise the bounds my be too
      // small. Be careful to read it here before deallocating the metadata!
      auto p = slab_metadata.chunk_ptr;

      local_state.get_meta_range().dealloc_range(
        capptr::Chunk<void>(&slab_metadata), sizeof(SlabMetadata));

      local_state.object_range.dealloc_range(p, size);
    }

    static size_t get_current_usage()
    {
      Stats stats_state;
      return stats_state.get_current_usage();
    }

    static size_t get_peak_usage()
    {
      Stats stats_state;
      return stats_state.get_peak_usage();
    }

    static SNMALLOC_FAST_PATH capptr::Alloc<void> reversion_alloc(
    capptr::Alloc<void> p_tame, const typename Pagemap::Entry& entry) {
      CapPtr<void, capptr::bounds::Alloc> rederived_ptr;
      size_t len = sizeclass_full_to_size(entry.get_sizeclass());
      /*
        * Attempt to set bounds of p to size of sizeclass. Assuming p is
        * derived from a correctly bounded pointer allocated by snmalloc this
        * will result in an error if either:
        * 1) p does not point to the start of the allocation (non-zero offset)
        * 2) the bounds of p do not encompass the entire allocation, as they
        *    should have when returned by alloc.
        * The error will either be a trap or a cleared tag depending on the
        * architecture. If the tag is clared this will be detected by
        * camcdecversion below.
        */
      capptr::Alloc<void> p_tame2 = Aal::capptr_bound<void, capptr::bounds::Alloc>(p_tame, len); // XXX use of p_wild here
      /*
        * Derive a pointer from chunk_ptr with address of p. This provides the
        * the authority to perform camcdecversion and storeversion. getaddr
        * / setaddr might be a more natural way to do this but we already have
        * pointer_diff / pointer_offset and this should work.
        */
      auto chunk_ptr = entry.get_slab_metadata()->chunk_ptr;
      size_t p_offset = pointer_diff(chunk_ptr, p_tame2);
      chunk_ptr = pointer_offset(chunk_ptr, p_offset);
      auto bounded_chunk_ptr = Aal::capptr_bound<void, capptr::bounds::AllocFull>(chunk_ptr, len);
      /**
       * Attempt to atomically decrement the version of the first granule of
       * the allocation.
       */
      AmoDecResult amoDecResult = Aal::capptr_tint_amo_dec(bounded_chunk_ptr, p_tame2);
      switch (amoDecResult) {
        case AmoDecResult::Reuse: {
          // Camocdecversion succeeded
          tint_t old_ver = Aal::capptr_tint_get(p_tame2);
          tint_t new_ver = old_ver - 1;
          auto reversioned_ptr = capptr_tint_region<true>(bounded_chunk_ptr, len, new_ver);
          rederived_ptr = capptr_to_user_address_control(reversioned_ptr);
          break;
        }
        case AmoDecResult::Quarantine: {
          // XXX instant revoke!
          auto reversioned_ptr = capptr_tint_region<false>(bounded_chunk_ptr, len, 15);
          rederived_ptr = capptr_to_user_address_control(reversioned_ptr);
          break;
        }
        case AmoDecResult::Fail:
          Pal::error("Version mismatch on dealloc: double free?");
      }
      return rederived_ptr;
    }
  };
} // namespace snmalloc
