#pragma once
#include "../ds/concept.h"
#include "../mem/allocconfig.h" /* TODO: CACHELINE_SIZE */
#include "../pal/pal_concept.h"

namespace snmalloc
{
  /**
   * A guaranteed type-stable sub-structure of all metadata referenced by the
   * Pagemap.  Use-specific structures (Metaslab, ChunkRecord) are expected to
   * have this at offset zero so that, even in the face of concurrent mutation
   * and reuse of the memory backing that metadata, the types of these fields
   * remain fixed.
   *
   * This class's data is fully private but is friends with the relevant backend
   * types and, thus, is "opaque" to the frontend.
   */
  class MetaCommon
  {
    friend class ChunkAllocator;

    template<SNMALLOC_CONCEPT(ConceptPAL) PAL, bool, typename>
    friend class BackendAllocator;

    capptr::Chunk<void> chunk;

  public:
    /**
     * Expose the address of, though not the authority to, our corresponding
     * chunk.
     */
    [[nodiscard]] SNMALLOC_FAST_PATH address_t chunk_address()
    {
      return address_cast(this->chunk);
    }

    /**
     * Zero (possibly by unmapping) the memory backing this chunk.  We must rely
     * on the caller to tell us its size, which is a little unfortunate.
     */
    template<SNMALLOC_CONCEPT(ConceptPAL) PAL>
    SNMALLOC_FAST_PATH void zero_chunk(size_t chunk_size)
    {
      PAL::zero(this->chunk.unsafe_ptr(), chunk_size);
    }
  };

  static const size_t PAGEMAP_METADATA_STRUCT_SIZE =
#ifdef __CHERI_PURE_CAPABILITY__
    2 * CACHELINE_SIZE
#else
    CACHELINE_SIZE
#endif
    ;

  // clang-format off
  /* This triggers an ICE (C1001) in MSVC, so disable it there */
#if defined(__cpp_concepts) && !defined(_MSC_VER)

  template<typename Meta>
  concept ConceptMetadataStruct =
    // Metadata structures must be standard layout (for offsetof()),
    std::is_standard_layout_v<Meta> &&
    // must be of a sufficiently small size,
    sizeof(Meta) <= PAGEMAP_METADATA_STRUCT_SIZE &&
    // and must be pointer-interconvertable with MetaCommon.
    (
      ConceptSame<Meta, MetaCommon> ||
      requires(Meta m) {
        // Otherwise, meta must have MetaCommon field named meta_common ...
        { &m.meta_common } -> ConceptSame<MetaCommon*>;
        // at offset zero.
        (offsetof(Meta, meta_common) == 0);
      }
    );

  static_assert(ConceptMetadataStruct<MetaCommon>);
#  define USE_METADATA_CONCEPT
#endif
  // clang-format on

  /**
   * Entry stored in the pagemap.  See docs/AddressSpace.md for the full
   * MetaEntry lifecycle.
   */
  class MetaEntry
  {
    template<typename Pagemap>
    friend class BuddyChunkRep;

    /**
     * In common cases, the pointer to the metaslab.  See docs/AddressSpace.md
     * for additional details.
     *
     * The bottom bit is used to indicate if this is the first chunk in a PAL
     * allocation, that cannot be combined with the preceeding chunk.
     */
    uintptr_t meta{0};

    /**
     * In common cases, a bit-packed pointer to the owning allocator (if any),
     * and the sizeclass of this chunk.  See mem/metaslab.h:MetaEntryRemote for
     * details of this case and docs/AddressSpace.md for further details.
     */
    uintptr_t remote_and_sizeclass{0};

  public:
    /**
     * Bit used to indicate this should not be considered part of the previous
     * PAL allocation.
     *
     * Some platforms cannot treat different PalAllocs as a single allocation.
     * This is true on CHERI as the combined permission might not be
     * representable.  It is also true on Windows as you cannot Commit across
     * multiple continuous VirtualAllocs.
     */
    static constexpr address_t META_BOUNDARY_BIT = 1 << 0;

    /**
     * This bit is set in remote_and_sizeclass to discriminate between the case
     * that it is in use by the frontend (0) or by the backend (1).  For the
     * former case, see mem/metaslab.h; for the latter, see backend/backend.h
     * and backend/largebuddyrange.h.
     *
     * This value is statically checked by the frontend to ensure that its
     * bit packing does not conflict; see mem/remoteallocator.h
     */
    static constexpr address_t REMOTE_BACKEND_MARKER = 1 << 7;

    constexpr MetaEntry() = default;

    /**
     * Constructor, provides the remote and sizeclass embedded in a single
     * pointer-sized word.  This format is not guaranteed to be stable and so
     * the second argument of this must always be the return value from
     * `get_remote_and_sizeclass`.
     */
    SNMALLOC_FAST_PATH
    MetaEntry(MetaCommon* meta, uintptr_t remote_and_sizeclass)
    : meta(unsafe_to_uintptr<MetaCommon>(meta)),
      remote_and_sizeclass(remote_and_sizeclass)
    {}

    /**
     * Return the remote and sizeclass in an implementation-defined encoding.
     * This is not guaranteed to be stable across snmalloc releases and so the
     * only safe use for this is to pass it to the two-argument constructor of
     * this class.
     */
    [[nodiscard]] SNMALLOC_FAST_PATH const uintptr_t&
    get_remote_and_sizeclass() const
    {
      return remote_and_sizeclass;
    }

    MetaEntry(const MetaEntry&) = delete;

    MetaEntry& operator=(const MetaEntry& other)
    {
      // Don't overwrite the boundary bit with the other's
      meta = (other.meta & ~META_BOUNDARY_BIT) |
        address_cast(meta & META_BOUNDARY_BIT);
      remote_and_sizeclass = other.remote_and_sizeclass;
      return *this;
    }

    /**
     * Return the Metaslab metadata associated with this chunk, guarded by an
     * assert that this chunk is being used as a slab (i.e., has an associated
     * owning allocator).
     */
    [[nodiscard]] SNMALLOC_FAST_PATH MetaCommon* get_meta() const
    {
      return reinterpret_cast<MetaCommon*>(meta & ~META_BOUNDARY_BIT);
    }

    void set_boundary()
    {
      meta |= META_BOUNDARY_BIT;
    }

    [[nodiscard]] bool is_boundary() const
    {
      return meta & META_BOUNDARY_BIT;
    }

    bool clear_boundary_bit()
    {
      return meta &= ~META_BOUNDARY_BIT;
    }
  };
} // namespace snmalloc
