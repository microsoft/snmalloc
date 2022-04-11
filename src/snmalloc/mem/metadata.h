#pragma once

#include "../ds/ds.h"
#include "freelist.h"
#include "sizeclasstable.h"

namespace snmalloc
{
  struct RemoteAllocator;

  /**
   * Remotes need to be aligned enough that the bottom bits have enough room for
   * all the size classes, both large and small. An additional bit is required
   * to separate backend uses.
   */
  static constexpr size_t REMOTE_MIN_ALIGN =
    bits::max<size_t>(CACHELINE_SIZE, SIZECLASS_REP_SIZE) << 1;

  /**
   * Base class for the templated FrontendMetaEntry.  This exists to avoid
   * needing a template parameter to access constants that are independent of
   * the template parameter and contains all of the state that is agnostic to
   * the types used for storing per-slab metadata.  This class should never be
   * instantiated directly (and its protected constructor guarantees that),
   * only the templated subclass should be use.  The subclass provides
   * convenient accessors.
   *
   * A back end may also subclass `FrontendMetaEntry` to provide other
   * back-end-specific information.  The front end never directly instantiates
   * these.
   */
  class MetaEntryBase
  {
  protected:
    /**
     * This bit is set in remote_and_sizeclass to discriminate between the case
     * that it is in use by the frontend (0) or by the backend (1).  For the
     * former case, see other methods on this and the subclass
     * `FrontendMetaEntry`; for the latter, see backend/backend.h and
     * backend/largebuddyrange.h.
     *
     * This value is statically checked by the frontend to ensure that its
     * bit packing does not conflict; see mem/remoteallocator.h
     */
    static constexpr address_t REMOTE_BACKEND_MARKER = 1 << 7;

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
     * The bit above the sizeclass is always zero unless this is used
     * by the backend to represent another datastructure such as the buddy
     * allocator entries.
     */
    static constexpr size_t REMOTE_WITH_BACKEND_MARKER_ALIGN =
      MetaEntryBase::REMOTE_BACKEND_MARKER;
    static_assert(
      (REMOTE_MIN_ALIGN >> 1) == MetaEntryBase::REMOTE_BACKEND_MARKER);

    /**
     * In common cases, the pointer to the slab metadata.  See
     * docs/AddressSpace.md for additional details.
     *
     * The bottom bit is used to indicate if this is the first chunk in a PAL
     * allocation, that cannot be combined with the preceeding chunk.
     */
    uintptr_t meta{0};

    /**
     * In common cases, a bit-packed pointer to the owning allocator (if any),
     * and the sizeclass of this chunk.  See `encode` for
     * details of this case and docs/AddressSpace.md for further details.
     */
    uintptr_t remote_and_sizeclass{0};

    /**
     * Constructor from two pointer-sized words.  The subclass is responsible
     * for ensuring that accesses to these are type-safe.
     */
    constexpr MetaEntryBase(uintptr_t m, uintptr_t ras)
    : meta(m), remote_and_sizeclass(ras)
    {}

    /**
     * Default constructor, zero initialises.
     */
    constexpr MetaEntryBase() : MetaEntryBase(0, 0) {}

    /**
     * When a meta entry is in use by the back end, it exposes two words of
     * state.  The low bits in both are reserved.  Bits in this bitmask must
     * not be set by the back end in either word.
     *
     * During a major release, this constraint may be weakened, allowing the
     * back end to set more bits.  We don't currently use all of these bits in
     * both words, but we reserve them all to make access uniform.  If more
     * bits are required by a back end then we could make this asymmetric.
     *
     * `REMOTE_BACKEND_MARKER` is the highest bit that we reserve, so this is
     * currently every bit including that bit and all lower bits.
     */
    static constexpr address_t BACKEND_RESERVED_MASK =
      (REMOTE_BACKEND_MARKER << 1) - 1;

  public:
    /**
     * Does the back end currently own this entry?  Note that freshly
     * allocated entries are owned by the front end until explicitly
     * claimed by the back end and so this will return `false` if neither
     * the front nor back end owns this entry.
     */
    [[nodiscard]] bool is_backend_owned() const
    {
      return (REMOTE_BACKEND_MARKER & remote_and_sizeclass) ==
        REMOTE_BACKEND_MARKER;
    }

    /**
     * Returns true if this metaentry has not been claimed by the front or back
     * ends.
     */
    [[nodiscard]] bool is_unowned() const
    {
      return ((meta == 0) || (meta == META_BOUNDARY_BIT)) &&
        (remote_and_sizeclass == 0);
    }

    /**
     * Encode the remote and the sizeclass.
     */
    [[nodiscard]] static SNMALLOC_FAST_PATH uintptr_t
    encode(RemoteAllocator* remote, sizeclass_t sizeclass)
    {
      /* remote might be nullptr; cast to uintptr_t before offsetting */
      return pointer_offset(
        reinterpret_cast<uintptr_t>(remote), sizeclass.raw());
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

    /**
     * Explicit assignment operator, copies the data preserving the boundary bit
     * in the target if it is set.
     */
    MetaEntryBase& operator=(const MetaEntryBase& other)
    {
      // Don't overwrite the boundary bit with the other's
      meta = (other.meta & ~META_BOUNDARY_BIT) |
        address_cast(meta & META_BOUNDARY_BIT);
      remote_and_sizeclass = other.remote_and_sizeclass;
      return *this;
    }

    /**
     * On some platforms, allocations originating from the OS may not be
     * combined.  The boundary bit indicates whether this is meta entry
     * corresponds to the first chunk in such a range and so may not be combined
     * with anything before it in the address space.
     * @{
     */
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
    ///@}

    /**
     * Returns the remote.
     *
     * If the meta entry is owned by the back end then this returns an
     * undefined value and will abort in debug builds.
     */
    [[nodiscard]] SNMALLOC_FAST_PATH RemoteAllocator* get_remote() const
    {
      SNMALLOC_ASSERT(!is_backend_owned());
      return reinterpret_cast<RemoteAllocator*>(
        pointer_align_down<REMOTE_WITH_BACKEND_MARKER_ALIGN>(
          get_remote_and_sizeclass()));
    }

    /**
     * Return the sizeclass.
     *
     * This can be called irrespective of whether the corresponding meta entry
     * is owned by the front or back end (and is, for example, called by
     * `external_pointer`). In the future, it may provide some stronger
     * guarantees on the value that is returned in this case.
     */
    [[nodiscard]] SNMALLOC_FAST_PATH sizeclass_t get_sizeclass() const
    {
      // TODO: perhaps remove static_cast with resolution of
      // https://github.com/CTSRD-CHERI/llvm-project/issues/588
      return sizeclass_t::from_raw(
        static_cast<size_t>(get_remote_and_sizeclass()) &
        (REMOTE_WITH_BACKEND_MARKER_ALIGN - 1));
    }

    /**
     * Claim the meta entry for use by the back end.  This preserves the
     * boundary bit, if it is set, but otherwise resets the meta entry to a
     * pristine state.
     */
    void claim_for_backend()
    {
      meta = is_boundary() ? META_BOUNDARY_BIT : 0;
      remote_and_sizeclass = REMOTE_BACKEND_MARKER;
    }

    /**
     * When used by the back end, the two words in a meta entry have no
     * semantics defined by the front end and are identified by enumeration
     * values.
     */
    enum class Word
    {
      /**
       * The first word.
       */
      One,

      /**
       * The second word.
       */
      Two
    };

    static constexpr bool is_backend_allowed_value(Word, uintptr_t val)
    {
      return (val & BACKEND_RESERVED_MASK) == 0;
    }

    /**
     * Proxy class that allows setting and reading back the bits in each word
     * that are exposed for the back end.
     *
     * The back end must not keep instances of this class after returning the
     * corresponding meta entry to the front end.
     */
    class BackendStateWordRef
    {
      /**
       * A pointer to the relevant word.
       */
      uintptr_t* val;

    public:
      /**
       * Constructor, wraps a `uintptr_t`.  Note that this may be used outside
       * of the meta entry by code wishing to provide uniform storage to things
       * that are either in a meta entry or elsewhere.
       */
      constexpr BackendStateWordRef(uintptr_t* v) : val(v) {}

      /**
       * Copy constructor.  Aliases the underlying storage.  Note that this is
       * not thread safe: two `BackendStateWordRef` instances sharing access to
       * the same storage must not be used from different threads without
       * explicit synchronisation.
       */
      constexpr BackendStateWordRef(const BackendStateWordRef& other) = default;

      /**
       * Read the value.  This zeroes any bits in the underlying storage that
       * the back end is not permitted to access.
       */
      [[nodiscard]] uintptr_t get() const
      {
        return (*val) & ~BACKEND_RESERVED_MASK;
      }

      /**
       * Default copy assignment.  See the copy constructor for constraints on
       * using this.
       */
      BackendStateWordRef&
      operator=(const BackendStateWordRef& other) = default;

      /**
       * Assignment operator.  Zeroes the bits in the provided value that the
       * back end is not permitted to use and then stores the result in the
       * value that this class manages.
       */
      BackendStateWordRef& operator=(uintptr_t v)
      {
        SNMALLOC_ASSERT_MSG(
          ((v & BACKEND_RESERVED_MASK) == 0),
          "The back end is not permitted to use the low bits in the meta "
          "entry. ({} & {}) == {}.",
          v,
          BACKEND_RESERVED_MASK,
          (v & BACKEND_RESERVED_MASK));
        *val = v | (static_cast<address_t>(*val) & BACKEND_RESERVED_MASK);
        return *this;
      }

      /**
       * Comparison operator.  Performs address comparison *not* value
       * comparison.
       */
      bool operator!=(const BackendStateWordRef& other) const
      {
        return val != other.val;
      }

      /**
       * Returns the address of the underlying storage in a form that can be
       * passed to `snmalloc::message` for printing.
       */
      address_t printable_address()
      {
        return address_cast(val);
      }
    };

    /**
     * Get a proxy that allows the back end to read from and write to (some bits
     * of) a word in the meta entry.  The meta entry must either be unowned or
     * explicitly claimed by the back end before calling this.
     */
    BackendStateWordRef get_backend_word(Word w)
    {
      if (!is_backend_owned())
      {
        SNMALLOC_ASSERT_MSG(
          is_unowned(),
          "Meta entry is owned by the front end.  Meta: {}, "
          "remote_and_sizeclass:{}",
          meta,
          remote_and_sizeclass);
        claim_for_backend();
      }
      return {w == Word::One ? &meta : &remote_and_sizeclass};
    }
  };

  /**
   * The FrontendSlabMetadata represent the metadata associated with a single
   * slab.
   */
  class alignas(CACHELINE_SIZE) FrontendSlabMetadata
  {
  public:
    /**
     * Used to link slab metadata together in various other data-structures.
     * This is intended to be used with `SeqSet` and so may actually hold a
     * subclass of this class provided by the back end.  The `SeqSet` is
     * responsible for maintaining that invariant.  While an instance of this
     * class is in a `SeqSet<T>`, the `next` field should not be assigned to by
     * anything that doesn't enforce the invariant that `next` stores a `T*`,
     * where `T` is a subclass of `FrontendSlabMetadata`.
     */
    FrontendSlabMetadata* next{nullptr};

    constexpr FrontendSlabMetadata() = default;

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
     * Initialise FrontendSlabMetadata for a slab.
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
     * Try to set this slab metadata to sleep.  If the remaining elements are
     * fewer than the threshold, then it will actually be set to the sleeping
     * state, and will return true, otherwise it will return false.
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
     * specifies if the slab metadata should be placed in the queue for that
     * sizeclass.
     *
     * If Randomisation is not used, it will always return false for the second
     * component, but with randomisation, it may only return part of the
     * available objects for this slab metadata.
     */
    template<typename Domesticator>
    static SNMALLOC_FAST_PATH std::pair<freelist::HeadPtr, bool>
    alloc_free_list(
      Domesticator domesticate,
      FrontendSlabMetadata* meta,
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

  /**
   * Entry stored in the pagemap.  See docs/AddressSpace.md for the full
   * FrontendMetaEntry lifecycle.
   */
  template<typename BackendSlabMetadata>
  class FrontendMetaEntry : public MetaEntryBase
  {
    /**
     * Ensure that the template parameter is valid.
     */
    static_assert(
      std::is_convertible_v<BackendSlabMetadata, FrontendSlabMetadata>,
      "The front end requires that the back end provides slab metadata that is "
      "compatible with the front-end's structure");

  public:
    constexpr FrontendMetaEntry() = default;

    /**
     * Constructor, provides the remote and sizeclass embedded in a single
     * pointer-sized word.  This format is not guaranteed to be stable and so
     * the second argument of this must always be the return value from
     * `get_remote_and_sizeclass`.
     */
    SNMALLOC_FAST_PATH
    FrontendMetaEntry(BackendSlabMetadata* meta, uintptr_t remote_and_sizeclass)
    : MetaEntryBase(
        unsafe_to_uintptr<BackendSlabMetadata>(meta), remote_and_sizeclass)
    {
      SNMALLOC_ASSERT_MSG(
        (REMOTE_BACKEND_MARKER & remote_and_sizeclass) == 0,
        "Setting a backend-owned value ({}) via the front-end interface is not "
        "allowed",
        remote_and_sizeclass);
      remote_and_sizeclass &= ~REMOTE_BACKEND_MARKER;
    }

    /**
     * Implicit copying of meta entries is almost certainly a bug and so the
     * copy constructor is deleted to statically catch these problems.
     */
    FrontendMetaEntry(const FrontendMetaEntry&) = delete;

    /**
     * Explicit assignment operator, copies the data preserving the boundary bit
     * in the target if it is set.
     */
    FrontendMetaEntry& operator=(const FrontendMetaEntry& other)
    {
      MetaEntryBase::operator=(other);
      return *this;
    }

    /**
     * Return the FrontendSlabMetadata metadata associated with this chunk,
     * guarded by an assert that this chunk is being used as a slab (i.e., has
     * an associated owning allocator).
     */
    [[nodiscard]] SNMALLOC_FAST_PATH BackendSlabMetadata*
    get_slab_metadata() const
    {
      SNMALLOC_ASSERT(get_remote() != nullptr);
      return unsafe_from_uintptr<BackendSlabMetadata>(
        meta & ~META_BOUNDARY_BIT);
    }
  };

} // namespace snmalloc
