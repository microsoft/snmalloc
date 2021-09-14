#pragma once

#include "../ds/dllist.h"
#include "../ds/helpers.h"
#include "../ds/seqqueue.h"
#include "../mem/remoteallocator.h"
#include "freelist.h"
#include "sizeclasstable.h"

namespace snmalloc
{
  class Slab;

  // The Metaslab represent the status of a single slab.
  // This can be either a short or a standard slab.
  class alignas(CACHELINE_SIZE) Metaslab
  {
  public:
    // Used to link metaslabs together in various other data-structures.
    Metaslab* next{nullptr};

    constexpr Metaslab() = default;

    /**
     *  Data-structure for building the free list for this slab.
     */
#ifdef SNMALLOC_CHECK_CLIENT
    FreeListBuilder<true, capptr::bounds::Alloc, capptr::bounds::AllocWild>
      free_queue;
#else
    FreeListBuilder<false, capptr::bounds::Alloc, capptr::bounds::AllocWild>
      free_queue;
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
      set_sleeping(sizeclass, 0);
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

    /**
     * Try to set this metaslab to sleep.  If the remaining elements are fewer
     * than the threshold, then it will actually be set to the sleeping state,
     * and will return true, otherwise it will return false.
     */
    SNMALLOC_FAST_PATH bool
    set_sleeping(sizeclass_t sizeclass, uint16_t remaining)
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
    static SNMALLOC_FAST_PATH std::pair<
      FreeObject::HeadPtr<capptr::bounds::Alloc, capptr::bounds::AllocWild>,
      bool>
    alloc_free_list(
      Domesticator domesticate,
      Metaslab* meta,
      FreeListIter<capptr::bounds::Alloc, capptr::bounds::AllocWild>&
        fast_free_list,
      LocalEntropy& entropy,
      sizeclass_t sizeclass)
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

  struct MetaslabCache
  {
    SeqQueue<Metaslab> queue;
    uint16_t unused = 0;
    uint16_t length = 0;
  };

} // namespace snmalloc
