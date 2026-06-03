#pragma once

#include "../ds/ds.h"
#include "freelist.h"
#include "snmalloc/stl/new.h"

namespace snmalloc
{
  struct RemoteAllocator;

  /**
   * RemoteAllocator pointers must have their low `COMBINED_BITS` zero
   * so the (sizeclass, offset) field can be OR-ed in by `encode`.
   */
  static constexpr size_t REMOTE_MIN_ALIGN =
    bits::max<size_t>(CACHELINE_SIZE, COMBINED_REP_SIZE);

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
     * Low bits of `remote_and_sizeclass` holding the sizeclass alone.
     */
    static constexpr address_t SIZECLASS_MASK = SIZECLASS_REP_SIZE - 1;

    /**
     * Low bits of `remote_and_sizeclass` holding the (sizeclass, offset)
     * pair. Also the markerless ownership discriminator:
     * `(ras & COMBINED_MASK) == 0` iff the entry is NOT in active
     * frontend use (frontend entries always have sizeclass != 0; slot 0
     * is the unmapped sentinel).
     */
    static constexpr address_t COMBINED_MASK = COMBINED_REP_SIZE - 1;

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
     * Alignment used by `get_remote` to mask off the (sizeclass, offset)
     * bits and recover the `RemoteAllocator*` payload.
     */
    static constexpr size_t REMOTE_WITH_BACKEND_MARKER_ALIGN =
      COMBINED_REP_SIZE;
    static_assert(REMOTE_MIN_ALIGN >= COMBINED_REP_SIZE);

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
     * Per-word frontend-reserved masks. Bits in these masks are owned by
     * the frontend; the backend must preserve them on writes (enforced
     * by `BackendStateWordRef::operator=`).
     *
     * - Word::One reserves `META_BOUNDARY_BIT` so PAL-allocation
     *   boundaries survive ownership transitions.
     * - Word::Two reserves `COMBINED_MASK`; the markerless ownership
     *   discriminator requires these bits to be zero in backend mode,
     *   and backend writes here are chunk-aligned so the requirement
     *   is naturally satisfied.
     */
    static constexpr address_t BACKEND_RESERVED_MASK_WORD_ONE =
      META_BOUNDARY_BIT;
    static constexpr address_t BACKEND_RESERVED_MASK_WORD_TWO = COMBINED_MASK;

  public:
    /**
     * First bit on Word::One available for backend layouts; the bits
     * below are frontend-reserved. Backends in `largearenarange.h`
     * derive `RED_BIT`, `VARIANT_SHIFT`, etc. from this.
     */
    static constexpr size_t BACKEND_LAYOUT_FIRST_FREE_BIT = 1;

    /**
     * True iff this entry is not in active frontend use (backend-claimed
     * or untouched). Frontend entries always have `sizeclass != 0`
     * (slot 0 is the unmapped sentinel), so the discriminator
     * `(ras & COMBINED_MASK) == 0` distinguishes them.
     */
    [[nodiscard]] bool is_backend_owned() const
    {
      return (remote_and_sizeclass & COMBINED_MASK) == 0;
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
     * Pack `remote`, `sizeclass`, and the per-chunk slab offset into a
     * `remote_and_sizeclass` word. `offset` defaults to 0; the backend's
     * multi-slab-tile write loop in `alloc_chunk` overrides it with the
     * chunk's slab index so `start_of_object` can recover the
     * allocation base.
     */
    [[nodiscard]] static SNMALLOC_FAST_PATH uintptr_t
    encode(RemoteAllocator* remote, sizeclass_t sizeclass, size_t offset = 0)
    {
      /* remote might be nullptr; cast to uintptr_t before offsetting */
      return pointer_offset(
        reinterpret_cast<uintptr_t>(remote),
        offset_and_sizeclass_t(sizeclass, offset).raw());
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
     *
     * Load-bearing: the pagemap writes back through this operator (its
     * `set(p, t)` is `body[p >> SHIFT] = t`), so the boundary bit set
     * once at OS-range registration survives every subsequent metadata
     * mutation — including chunk reuse via `dealloc_chunk` — without
     * any consolidation path having to touch it explicitly.
     */
    MetaEntryBase& operator=(const MetaEntryBase& other)
    {
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
     * Return the `RemoteAllocator*` payload by masking off the low
     * `COMBINED_BITS`. Callable in any state: for unowned entries
     * yields nullptr; for backend-owned entries yields a chunk address
     * which compares unequal to any allocator's `public_state()`, so
     * dispatch falls through to the slow path.
     */
    [[nodiscard]] SNMALLOC_FAST_PATH RemoteAllocator* get_remote() const
    {
      return reinterpret_cast<RemoteAllocator*>(
        pointer_align_down<REMOTE_WITH_BACKEND_MARKER_ALIGN>(
          get_remote_and_sizeclass()));
    }

    /**
     * Returns true if this memory is owned by snmalloc.  Some backend memory
     * may return false, but all frontend memory will return true.
     */
    [[nodiscard]] SNMALLOC_FAST_PATH bool is_owned() const
    {
      return get_remote() != nullptr;
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
        static_cast<size_t>(get_remote_and_sizeclass()) & SIZECLASS_MASK);
    }

    /**
     * Return the (sizeclass, slab offset) pair indexing
     * `sizeclass_metadata.start_`. The selected row carries
     * `offset_bytes = offset * slab_size` precomputed, so
     * `start_of_object` recovers the allocation base with a single
     * subtract.
     */
    [[nodiscard]] SNMALLOC_FAST_PATH offset_and_sizeclass_t
    get_offset_and_sizeclass() const
    {
      return offset_and_sizeclass_t::from_raw(
        static_cast<size_t>(get_remote_and_sizeclass()) & COMBINED_MASK);
    }

    /**
     * Claim the meta entry for the backend: preserves the boundary bit
     * and zeros `remote_and_sizeclass` so `is_backend_owned()` holds.
     */
    void claim_for_backend()
    {
      meta = is_boundary() ? META_BOUNDARY_BIT : 0;
      remote_and_sizeclass = 0;
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

    static constexpr bool is_backend_allowed_value(Word w, uintptr_t val)
    {
      const address_t mask = (w == Word::One) ? BACKEND_RESERVED_MASK_WORD_ONE :
                                                BACKEND_RESERVED_MASK_WORD_TWO;
      return (val & mask) == 0;
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

      /**
       * The frontend-reserved mask for the word that `val` points at. Bits
       * in this mask are owned by the frontend: `get()` clears them on
       * read, and `operator=` preserves them on write (by OR-ing the
       * current value's masked bits into the new value).
       */
      address_t reserved_mask{0};

    public:
      /**
       * Uninitialised constructor.
       */
      BackendStateWordRef() = default;

      /**
       * Constructor, wraps a `uintptr_t` and the frontend-reserved mask
       * that applies to that word. Note that this may be used outside of
       * the meta entry by code wishing to provide uniform storage to
       * things that are either in a meta entry or elsewhere.
       */
      constexpr BackendStateWordRef(uintptr_t* v, address_t mask)
      : val(v), reserved_mask(mask)
      {}

      /**
       * Single-pointer constructor required by the `RBRepMethods`
       * concept, which constructs a Handle from `&Rep::root` to
       * verify sentinel constructibility (see
       * `ds_core/redblacktree.h`). Reserved mask is zero, which is
       * safe because `Rep::root` is a `static const` sentinel that
       * the red-black tree never assigns through — any write would
       * trap on the const data — and on read the underlying value is
       * zero so `get()` returns zero regardless of the mask.
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
        return (*val) & ~reserved_mask;
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
          ((v & reserved_mask) == 0),
          "The back end is not permitted to use the reserved bits in the "
          "meta entry. ({} & {}) == {}.",
          v,
          reserved_mask,
          (v & reserved_mask));
        *val = v | (static_cast<address_t>(*val) & reserved_mask);
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
      return (w == Word::One) ?
        BackendStateWordRef{&meta, BACKEND_RESERVED_MASK_WORD_ONE} :
        BackendStateWordRef{
          &remote_and_sizeclass, BACKEND_RESERVED_MASK_WORD_TWO};
    }
  };

  /**
   * FrontendSlabMetadata_Trait
   *
   * Used for static checks of inheritance as FrontendSlabMetadata is templated.
   */
  class FrontendSlabMetadata_Trait
  {
  private:
    template<typename BackendType, typename ClientMeta_>
    friend class FrontendSlabMetadata;

    // Can only be constructed by FrontendSlabMetadata
    constexpr FrontendSlabMetadata_Trait() = default;
  };

  /**
   * The FrontendSlabMetadata represent the metadata associated with a single
   * slab.
   */
  template<typename BackendType, typename ClientMeta_>
  class FrontendSlabMetadata : public FrontendSlabMetadata_Trait
  {
  public:
    /**
     * Type that encapsulates logic for accessing client meta-data.
     */
    using ClientMeta = ClientMeta_;

    /**
     * Used to link slab metadata together in various other data-structures.
     * This is used with `SeqSet` and so may actually hold a subclass of this
     * class provided by the back end.  The `SeqSet` is responsible for
     * maintaining that invariant.
     */
    typename SeqSet<BackendType>::Node node;

    constexpr FrontendSlabMetadata() = default;

    /**
     *  Data-structure for building the free list for this slab.
     */
    SNMALLOC_NO_UNIQUE_ADDRESS freelist::Builder<mitigations(random_preserve)>
      free_queue;

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
     * I.e. it is not in a Allocator cache for the appropriate sizeclass.
     */
    bool sleeping_ = false;

    /**
     * Flag to indicate this is actually a large allocation rather than a slab
     * of small allocations.
     */
    bool large_ = false;

    /**
     * Stores client meta-data for this slab. This must be last element in the
     * slab. The meta data will actually allocate multiple elements after this
     * type, so that client_meta_[1] will work for the required meta-data size.
     */
    SNMALLOC_NO_UNIQUE_ADDRESS typename ClientMeta::StorageType client_meta_{};

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
    void initialise(
      smallsizeclass_t sizeclass, address_t slab, const FreeListKey& key)
    {
      static_assert(
        stl::is_base_of_v<FrontendSlabMetadata_Trait, BackendType>,
        "Template should be a subclass of FrontendSlabMetadata");
      free_queue.init(slab, key, this->as_key_tweak());
      // Set up meta data as if the entire slab has been turned into a free
      // list. This means we don't have to check for special cases where we have
      // returned all the elements, but this is a slab that is still being bump
      // allocated from. Hence, the bump allocator slab will never be returned
      // for use in another size class.
      set_sleeping(sizeclass, 0);

      large_ = false;

      new (&client_meta_, placement_token)
        typename ClientMeta::StorageType[get_client_storage_count(sizeclass)];
    }

    /**
     * Make this a chunk represent a large allocation.
     *
     * Set needed so immediately moves to slow path.
     */
    void initialise_large(address_t slab, const FreeListKey& key)
    {
      // We will push to this just to make the fast path clean.
      free_queue.init(slab, key, this->as_key_tweak());

      // Flag to detect that it is a large alloc on the slow path
      large_ = true;

      // Jump to slow path on first deallocation.
      needed() = 1;

      new (&client_meta_, placement_token) typename ClientMeta::StorageType();
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

    class ReturnObjectsIterator
    {
      uint16_t _batch;
      FrontendSlabMetadata* _meta;

      static_assert(sizeof(_batch) * 8 > MAX_CAPACITY_BITS);

    public:
      ReturnObjectsIterator(uint16_t n, FrontendSlabMetadata* m)
      : _batch(n), _meta(m)
      {}

      template<bool first>
      SNMALLOC_FAST_PATH bool step()
      {
        // The first update must always return some positive number of objects.
        SNMALLOC_ASSERT(!first || (_batch != 0));

        /*
         * Stop iteration when there are no more objects to return.  Perform
         * this test only on non-first steps to avoid a branch on the hot path.
         */
        if (!first && _batch == 0)
          return false;

        if (SNMALLOC_LIKELY(_batch < _meta->needed()))
        {
          // Will not hit threshold for state transition
          _meta->needed() -= _batch;
          return false;
        }

        // Hit threshold for state transition, may yet hit another
        _batch -= _meta->needed();
        _meta->needed() = 0;
        return true;
      }
    };

    /**
     * A batch version of return_object.
     *
     * Returns an iterator that should have `.step<>()` called on it repeatedly
     * until it returns `false`.  The first step should invoke `.step<true>()`
     * while the rest should invoke `.step<false>()`.  After each
     * true-returning `.step()`, the caller should run the slow-path code to
     * update the rest of the metadata for this slab.
     */
    ReturnObjectsIterator return_objects(uint16_t n)
    {
      return ReturnObjectsIterator(n, this);
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
    static SNMALLOC_FAST_PATH stl::Pair<freelist::HeadPtr, bool>
    alloc_free_list(
      Domesticator domesticate,
      FrontendSlabMetadata* meta,
      freelist::Iter<>& fast_free_list,
      LocalEntropy& entropy,
      smallsizeclass_t sizeclass)
    {
      auto& key = freelist::Object::key_root;

      stl::remove_reference_t<decltype(fast_free_list)> tmp_fl;

      auto remaining =
        meta->free_queue.close(tmp_fl, key, meta->as_key_tweak());
      auto p = tmp_fl.take(key, domesticate);
      fast_free_list = tmp_fl;

      if constexpr (mitigations(random_preserve))
        entropy.refresh_bits();
      else
        UNUSED(entropy);

      // This marks the slab as sleeping, and sets a wakeup
      // when sufficient deallocations have occurred to this slab.
      // Takes how many deallocations were not grabbed on this call
      // This will be zero if there is no randomisation.
      auto sleeping = meta->set_sleeping(sizeclass, remaining);

      return {p, !sleeping};
    }

    // Returns a pointer to somewhere in the slab. May not be the
    // start of the slab.
    [[nodiscard]] address_t get_slab_interior(const FreeListKey& key) const
    {
      return address_cast(free_queue.read_head(0, key, this->as_key_tweak()));
    }

    [[nodiscard]] SNMALLOC_FAST_PATH address_t as_key_tweak() const noexcept
    {
      return as_key_tweak(address_cast(this));
    }

    [[nodiscard]] SNMALLOC_FAST_PATH static address_t
    as_key_tweak(address_t self)
    {
      return self / alignof(FrontendSlabMetadata);
    }

    typename ClientMeta::DataRef get_meta_for_object(size_t index)
    {
      return ClientMeta::get(&client_meta_, index);
    }

    static size_t get_client_storage_count(smallsizeclass_t sizeclass)
    {
      auto count = sizeclass_to_slab_object_count(sizeclass);
      auto result = ClientMeta::required_count(count);
      if (result == 0)
        return 1;
      return result;
    }

    static size_t get_extra_bytes(sizeclass_t sizeclass)
    {
      if (sizeclass.is_small())
        // We remove one from the extra-bytes as there is one in the metadata to
        // start with.
        return (get_client_storage_count(sizeclass.as_small()) - 1) *
          sizeof(typename ClientMeta::StorageType);

      // For large classes there is only a single entry, so this is covered by
      // the existing entry in the metaslab, and further bytes are not required.
      return 0;
    }
  };

  /**
   * Entry stored in the pagemap.  See docs/AddressSpace.md for the full
   * FrontendMetaEntry lifecycle.
   */
  template<typename SlabMetadataType>
  class FrontendMetaEntry : public MetaEntryBase
  {
    /**
     * Ensure that the template parameter is valid.
     */
    static_assert(
      stl::is_base_of_v<FrontendSlabMetadata_Trait, SlabMetadataType>,
      "Template should be a subclass of FrontendSlabMetadata");

  public:
    using SlabMetadata = SlabMetadataType;

    constexpr FrontendMetaEntry() = default;

    /**
     * Constructor, provides the remote and sizeclass embedded in a single
     * pointer-sized word.  This format is not guaranteed to be stable and so
     * the second argument of this must always be the return value from
     * `get_remote_and_sizeclass`.
     */
    SNMALLOC_FAST_PATH
    FrontendMetaEntry(SlabMetadata* meta, uintptr_t remote_and_sizeclass)
    : MetaEntryBase(unsafe_to_uintptr<SlabMetadata>(meta), remote_and_sizeclass)
    {}

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
     * Return the FrontendSlabMetadata pointer. Only meaningful when the
     * entry is frontend-owned; in other states the underlying word
     * holds tree-node fields. Callers must verify ownership first
     * (the standard idiom is `entry.get_remote() == self->public_state()`).
     */
    [[nodiscard]] SNMALLOC_FAST_PATH SlabMetadata* get_slab_metadata() const
    {
      return unsafe_from_uintptr<SlabMetadata>(meta & ~META_BOUNDARY_BIT);
    }
  };

} // namespace snmalloc
