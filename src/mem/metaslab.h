#pragma once

#include "../ds/cdllist.h"
#include "../ds/dllist.h"
#include "../ds/helpers.h"
#include "../mem/remoteallocator.h"
#include "freelist.h"
#include "ptrhelpers.h"
#include "sizeclasstable.h"

namespace snmalloc
{
  class Slab;

  using SlabLink = CDLLNode<>;

  // The Metaslab represent the status of a single slab.
  // This can be either a short or a standard slab.
  class alignas(CACHELINE_SIZE) Metaslab
  {
  public:
    // TODO: Annotate with CHERI subobject unbound for pointer arithmetic
    SlabLink link;

    constexpr Metaslab() : link(true) {}

    /**
     * Metaslab::link points at another link field.  To get the actual Metaslab,
     * use this encapsulation of the container-of logic.
     */
    static Metaslab* from_link(SlabLink* ptr);

    /**
     *  Data-structure for building the free list for this slab.
     *
     *  Spare 32bits are used for the fields in MetaslabEnd.
     */
#ifdef SNMALLOC_CHECK_CLIENT
    FreeListBuilder<true> free_queue;
#else
    FreeListBuilder<false> free_queue;
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
    void initialise(sizeclass_t sizeclass)
    {
      free_queue.init();
      // Set up meta data as if the entire slab has been turned into a free
      // list. This means we don't have to check for special cases where we have
      // returned all the elements, but this is a slab that is still being bump
      // allocated from. Hence, the bump allocator slab will never be returned
      // for use in another size class.
      set_sleeping(sizeclass);
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

    SNMALLOC_FAST_PATH void set_sleeping(sizeclass_t sizeclass)
    {
      SNMALLOC_ASSERT(free_queue.empty());

      // Set needed to at least one, possibly more so we only use
      // a slab when it has a reasonable amount of free elements
      needed() = threshold_for_waking_slab(sizeclass);
      sleeping() = true;
    }

    SNMALLOC_FAST_PATH void set_not_sleeping(sizeclass_t sizeclass)
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

    static SNMALLOC_FAST_PATH bool
    is_start_of_object(sizeclass_t sizeclass, address_t p)
    {
      return is_multiple_of_sizeclass(
        sizeclass,
        p - (bits::align_down(p, sizeclass_to_slab_size(sizeclass))));
    }

    /**
     * TODO
     */
    static SNMALLOC_FAST_PATH CapPtr<FreeObject, CBAlloc> alloc(
      Metaslab* meta,
      FreeListIter& fast_free_list,
      LocalEntropy& entropy,
      sizeclass_t sizeclass)
    {
      auto& key = entropy.get_free_list_key();

      FreeListIter tmp_fl;
      meta->free_queue.close(tmp_fl, key);
      auto p = tmp_fl.take(key);
      fast_free_list = tmp_fl;

#ifdef SNMALLOC_CHECK_CLIENT
      entropy.refresh_bits();
#else
      UNUSED(entropy);
#endif

      // Treat stealing the free list as allocating it all.
      // This marks the slab as sleeping, and sets a wakeup
      // when sufficient deallocations have occurred to this slab.
      meta->set_sleeping(sizeclass);

      return p;
    }
  };

  inline Metaslab* Metaslab::from_link(SlabLink* lptr)
  {
    return pointer_offset_signed<Metaslab>(
      lptr, -static_cast<ptrdiff_t>(offsetof(Metaslab, link)));
  }

  struct RemoteAllocator;

  /**
   * Entry stored in the pagemap.
   */
  class MetaEntry
  {
    Metaslab* meta{nullptr};
    uintptr_t remote_and_sizeclass{0};

  public:
    constexpr MetaEntry() = default;

    /**
     * Constructor, provides the remote and sizeclass embedded in a single
     * pointer-sized word.  This format is not guaranteed to be stable and so
     * the second argument of this must always be the return value from
     * `get_remote_and_sizeclass`.
     */
    MetaEntry(Metaslab* meta, uintptr_t remote_and_sizeclass)
    : meta(meta), remote_and_sizeclass(remote_and_sizeclass)
    {}

    MetaEntry(Metaslab* meta, RemoteAllocator* remote, sizeclass_t sizeclass)
    : meta(meta)
    {
      /* remote might be nullptr; cast to uintptr_t before offsetting */
      remote_and_sizeclass =
        pointer_offset(reinterpret_cast<uintptr_t>(remote), sizeclass);
    }

    [[nodiscard]] Metaslab* get_metaslab() const
    {
      return meta;
    }

    /**
     * Return the remote and sizeclass in an implementation-defined encoding.
     * This is not guaranteed to be stable across snmalloc releases and so the
     * only safe use for this is to pass it to the two-argument constructor of
     * this class.
     */
    uintptr_t get_remote_and_sizeclass()
    {
      return remote_and_sizeclass;
    }

    [[nodiscard]] RemoteAllocator* get_remote() const
    {
      return reinterpret_cast<RemoteAllocator*>(
        pointer_align_down<alignof(RemoteAllocator)>(remote_and_sizeclass));
    }

    [[nodiscard]] sizeclass_t get_sizeclass() const
    {
      return remote_and_sizeclass & (alignof(RemoteAllocator) - 1);
    }
  };

  struct MetaslabCache : public CDLLNode<>
  {
    uint16_t unused = 0;
    uint16_t length = 0;
  };

} // namespace snmalloc
