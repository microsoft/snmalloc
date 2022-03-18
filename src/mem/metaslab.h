#pragma once

#include "../ds/helpers.h"
#include "../ds/seqset.h"
#include "../mem/remoteallocator.h"
#include "freelist.h"
#include "sizeclasstable.h"

namespace snmalloc
{
  /**
   * A guaranteed type-stable sub-structure of all metadata referenced by the
   * Pagemap.  Use-specific structures (Metaslab, ChunkRecord) are expected to
   * have this at offset zero so that, even in the face of concurrent mutation
   * and reuse of the memory backing that metadata, the types of these fields
   * remain fixed.
   */
  struct MetaCommon
  {
    capptr::Chunk<void> chunk;
  };

  // The Metaslab represent the status of a single slab.
  class alignas(CACHELINE_SIZE) Metaslab
  {
  public:
    MetaCommon meta_common;

    // Used to link metaslabs together in various other data-structures.
    Metaslab* next{nullptr};

    constexpr Metaslab() = default;

    /**
     *  Data-structure for building the free list for this slab.
     */
#ifdef SNMALLOC_CHECK_CLIENT
    freelist::Builder<true> free_queue;
#else
    freelist::Builder<false> free_queue;
#endif

    /**
     * The number of deallocation required until we hit a slow path. This
     * counts down in two different ways that are handled the same on the
     * fast path.  The first is
     *   - deallocations until the slab has sufficient entries to be considered
     *   useful to allocate from.  This could be as low as 1, or when we have
     *   a requirement for entropy then it could be much higher.
     *   - deallocations until the slab is completely unused.  This is needed
     *   to be detected, so that the statistics can be kept up to date, and
     *   potentially return memory to the a global pool of slabs/chunks.
     */
    uint16_t needed_ = 0;

    /**
     * Flag that is used to indicate that the slab is currently not active.
     * I.e. it is not in a CoreAllocator cache for the appropriate sizeclass.
     */
    bool sleeping_ = false;

    /**
     * Flag to indicate this is actually a large allocation rather than a slab
     * of small allocations.
     */
    bool large_ = false;

    uint16_t& needed()
    {
      return needed_;
    }

    bool& sleeping()
    {
      return sleeping_;
    }

    /**
     * Initialise Metaslab for a slab.
     */
    void initialise(smallsizeclass_t sizeclass)
    {
      free_queue.init();
      // Set up meta data as if the entire slab has been turned into a free
      // list. This means we don't have to check for special cases where we have
      // returned all the elements, but this is a slab that is still being bump
      // allocated from. Hence, the bump allocator slab will never be returned
      // for use in another size class.
      set_sleeping(sizeclass, 0);

      large_ = false;
    }

    /**
     * Make this a chunk represent a large allocation.
     *
     * Set needed so immediately moves to slow path.
     */
    void initialise_large()
    {
      // We will push to this just to make the fast path clean.
      free_queue.init();

      // Flag to detect that it is a large alloc on the slow path
      large_ = true;

      // Jump to slow path on first deallocation.
      needed() = 1;
    }

    /**
     * Updates statistics for adding an entry to the free list, if the
     * slab is either
     *  - empty adding the entry to the free list, or
     *  - was full before the subtraction
     * this returns true, otherwise returns false.
     */
    bool return_object()
    {
      return (--needed()) == 0;
    }

    bool is_unused()
    {
      return needed() == 0;
    }

    bool is_sleeping()
    {
      return sleeping();
    }

    bool is_large()
    {
      return large_;
    }

    /**
     * Try to set this metaslab to sleep.  If the remaining elements are fewer
     * than the threshold, then it will actually be set to the sleeping state,
     * and will return true, otherwise it will return false.
     */
    SNMALLOC_FAST_PATH bool
    set_sleeping(smallsizeclass_t sizeclass, uint16_t remaining)
    {
      auto threshold = threshold_for_waking_slab(sizeclass);
      if (remaining >= threshold)
      {
        // Set needed to at least one, possibly more so we only use
        // a slab when it has a reasonable amount of free elements
        auto allocated = sizeclass_to_slab_object_count(sizeclass);
        needed() = allocated - remaining;
        sleeping() = false;
        return false;
      }

      sleeping() = true;
      needed() = threshold - remaining;
      return true;
    }

    SNMALLOC_FAST_PATH void set_not_sleeping(smallsizeclass_t sizeclass)
    {
      auto allocated = sizeclass_to_slab_object_count(sizeclass);
      needed() = allocated - threshold_for_waking_slab(sizeclass);

      // Design ensures we can't move from full to empty.
      // There are always some more elements to free at this
      // point. This is because the threshold is always less
      // than the count for the slab
      SNMALLOC_ASSERT(needed() != 0);

      sleeping() = false;
    }

    /**
     * Allocates a free list from the meta data.
     *
     * Returns a freshly allocated object of the correct size, and a bool that
     * specifies if the metaslab should be placed in the queue for that
     * sizeclass.
     *
     * If Randomisation is not used, it will always return false for the second
     * component, but with randomisation, it may only return part of the
     * available objects for this metaslab.
     */
    template<typename Domesticator>
    static SNMALLOC_FAST_PATH std::pair<freelist::HeadPtr, bool>
    alloc_free_list(
      Domesticator domesticate,
      Metaslab* meta,
      freelist::Iter<>& fast_free_list,
      LocalEntropy& entropy,
      smallsizeclass_t sizeclass)
    {
      auto& key = entropy.get_free_list_key();

      std::remove_reference_t<decltype(fast_free_list)> tmp_fl;
      auto remaining = meta->free_queue.close(tmp_fl, key);
      auto p = tmp_fl.take(key, domesticate);
      fast_free_list = tmp_fl;

#ifdef SNMALLOC_CHECK_CLIENT
      entropy.refresh_bits();
#else
      UNUSED(entropy);
#endif

      // This marks the slab as sleeping, and sets a wakeup
      // when sufficient deallocations have occurred to this slab.
      // Takes how many deallocations were not grabbed on this call
      // This will be zero if there is no randomisation.
      auto sleeping = meta->set_sleeping(sizeclass, remaining);

      return {p, !sleeping};
    }
  };

  static_assert(std::is_standard_layout_v<Metaslab>);
  static_assert(
    offsetof(Metaslab, meta_common) == 0,
    "ChunkRecord and Metaslab must share a common prefix");

  /**
   * Entry stored in the pagemap.
   */
  class MetaEntry
  {
    template<typename Pagemap>
    friend class BuddyChunkRep;

    /**
     * The pointer to the metaslab, the bottom bit is used to indicate if this
     * is the first chunk in a PAL allocation, that cannot be combined with
     * the preceeding chunk.
     */
    uintptr_t meta{0};

    /**
     * Bit used to indicate this should not be considered part of the previous
     * PAL allocation.
     *
     * Some platforms cannot treat different PalAllocs as a single allocation.
     * This is true on CHERI as the combined permission might not be
     * representable.  It is also true on Windows as you cannot Commit across
     * multiple continuous VirtualAllocs.
     */
    static constexpr address_t BOUNDARY_BIT = 1;

    /**
     * A bit-packed pointer to the owning allocator (if any), and the sizeclass
     * of this chunk.  The sizeclass here is itself a union between two cases:
     *
     *  * log_2(size), at least MIN_CHUNK_BITS, for large allocations.
     *
     *  * a value in [0, NUM_SMALL_SIZECLASSES] for small allocations.  These
     * may be directly passed to the sizeclass (not slab_sizeclass) functions of
     *    sizeclasstable.h
     *
     */
    uintptr_t remote_and_sizeclass{0};

  public:
    constexpr MetaEntry() = default;

    /**
     * Constructor, provides the remote and sizeclass embedded in a single
     * pointer-sized word.  This format is not guaranteed to be stable and so
     * the second argument of this must always be the return value from
     * `get_remote_and_sizeclass`.
     */
    SNMALLOC_FAST_PATH
    MetaEntry(Metaslab* meta, uintptr_t remote_and_sizeclass)
    : meta(reinterpret_cast<uintptr_t>(meta)),
      remote_and_sizeclass(remote_and_sizeclass)
    {}

    SNMALLOC_FAST_PATH
    MetaEntry(
      Metaslab* meta,
      RemoteAllocator* remote,
      sizeclass_t sizeclass = sizeclass_t())
    : meta(reinterpret_cast<uintptr_t>(meta))
    {
      /* remote might be nullptr; cast to uintptr_t before offsetting */
      remote_and_sizeclass =
        pointer_offset(reinterpret_cast<uintptr_t>(remote), sizeclass.raw());
    }

    /**
     * Return the Metaslab metadata associated with this chunk, guarded by an
     * assert that this chunk is being used as a slab (i.e., has an associated
     * owning allocator).
     */
    [[nodiscard]] SNMALLOC_FAST_PATH Metaslab* get_metaslab() const
    {
      SNMALLOC_ASSERT(get_remote() != nullptr);
      return reinterpret_cast<Metaslab*>(meta & ~BOUNDARY_BIT);
    }

    /**
     * Return the remote and sizeclass in an implementation-defined encoding.
     * This is not guaranteed to be stable across snmalloc releases and so the
     * only safe use for this is to pass it to the two-argument constructor of
     * this class.
     */
    [[nodiscard]] SNMALLOC_FAST_PATH uintptr_t get_remote_and_sizeclass() const
    {
      return remote_and_sizeclass;
    }

    [[nodiscard]] SNMALLOC_FAST_PATH RemoteAllocator* get_remote() const
    {
      return reinterpret_cast<RemoteAllocator*>(
        pointer_align_down<REMOTE_WITH_BACKEND_MARKER_ALIGN>(
          remote_and_sizeclass));
    }

    [[nodiscard]] SNMALLOC_FAST_PATH sizeclass_t get_sizeclass() const
    {
      // TODO: perhaps remove static_cast with resolution of
      // https://github.com/CTSRD-CHERI/llvm-project/issues/588
      return sizeclass_t::from_raw(
        static_cast<size_t>(remote_and_sizeclass) &
        (REMOTE_WITH_BACKEND_MARKER_ALIGN - 1));
    }

    MetaEntry(const MetaEntry&) = delete;

    MetaEntry& operator=(const MetaEntry& other)
    {
      // Don't overwrite the boundary bit with the other's
      meta = (other.meta & ~BOUNDARY_BIT) | address_cast(meta & BOUNDARY_BIT);
      remote_and_sizeclass = other.remote_and_sizeclass;
      return *this;
    }

    void set_boundary()
    {
      meta |= BOUNDARY_BIT;
    }

    [[nodiscard]] bool is_boundary() const
    {
      return meta & BOUNDARY_BIT;
    }

    bool clear_boundary_bit()
    {
      return meta &= ~BOUNDARY_BIT;
    }
  };

  struct MetaslabCache
  {
#ifdef SNMALLOC_CHECK_CLIENT
    SeqSet<Metaslab> available;
#else
    // This is slightly faster in some cases,
    // but makes memory reuse more predictable.
    SeqSet<Metaslab, true> available;
#endif
    uint16_t unused = 0;
    uint16_t length = 0;
  };
} // namespace snmalloc
