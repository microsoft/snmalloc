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

  static_assert(
    sizeof(SlabLink) <= MIN_ALLOC_SIZE,
    "Need to be able to pack a SlabLink into any free small alloc");

  /**
   * This struct is used inside FreeListBuilder to account for the
   * alignment space that is wasted in sizeof.
   *
   * This is part of Metaslab abstraction.
   */
  struct MetaslabEnd
  {
    /**
     * TODO rewrite
     *  How many entries are not in the free list of slab, i.e.
     *  how many entries are needed to fully free this slab.
     *
     *  In the case of a fully allocated slab, where prev==0 needed
     *  will be 1. This enables 'return_object' to detect the slow path
     *  case with a single operation subtract and test.
     */
    uint16_t needed = 0;

    bool sleeping = false;
  };

  inline static size_t large_size_to_chunk_size(size_t size)
  {
    return bits::next_pow2(size);
  }

  inline static size_t large_size_to_chunk_sizeclass(size_t size)
  {
    return bits::next_pow2_bits(size) - MIN_CHUNK_BITS;
  }

  // The Metaslab represent the status of a single slab.
  // This can be either a short or a standard slab.
  class alignas(CACHELINE_SIZE) Metaslab : public SlabLink
  {
  public:
    constexpr Metaslab() : SlabLink(true) {}

    /**
     *  Data-structure for building the free list for this slab.
     *
     *  Spare 32bits are used for the fields in MetaslabEnd.
     */
#ifdef CHECK_CLIENT
    FreeListBuilder<true, MetaslabEnd> free_queue;
#else
    FreeListBuilder<false, MetaslabEnd> free_queue;
#endif

    uint16_t& needed()
    {
      return free_queue.s.needed;
    }

    bool& sleeping()
    {
      return free_queue.s.sleeping;
    }

    void initialise(sizeclass_t sizeclass)
    {
      // TODO: Special version for large alloc?
      //    TODO: Assert this is a Large alloc
      //    TODO: other fields for other data?

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
      FreeListIter tmp_fl;
      meta->free_queue.close(tmp_fl, entropy);
      auto p = tmp_fl.take(entropy);
      fast_free_list = tmp_fl;

#ifdef CHECK_CLIENT
      entropy.refresh_bits();
#endif

      // Treat stealing the free list as allocating it all.
      // This marks the slab as sleeping, and sets a wakeup
      // when sufficient deallocations have occurred to this slab.
      meta->set_sleeping(sizeclass);

      //    TODO?
      //      self->debug_slab_invariant(meta, entropy);

      // TODO: Should this be zeroing the FreeObject state?
      return p;
    }

    template<capptr_bounds B>
    void debug_slab_invariant(CapPtr<Slab, B> slab, LocalEntropy& entropy)
    {
      static_assert(B == CBChunkD || B == CBChunk);

#if false && !defined(NDEBUG) && !defined(SNMALLOC_CHEAP_CHECKS)
      bool is_short = Metaslab::is_short(slab);

      if (is_sleeping())
      {
        size_t count = free_queue.debug_length(entropy);
        SNMALLOC_ASSERT(count < threshold_for_waking_slab(sizeclass()));
        return;
      }

      if (is_unused())
        return;

      size_t size = sizeclass_to_size(sizeclass());
      size_t offset = get_initial_offset(sizeclass(), is_short);
      size_t accounted_for = needed() * size + offset;

      // Block is not full
      SNMALLOC_ASSERT(SLAB_SIZE > accounted_for);

      // Account for list size
      size_t count = free_queue.debug_length(entropy);
      accounted_for += count * size;

      SNMALLOC_ASSERT(count <= get_slab_capacity(sizeclass(), is_short));

      auto bumpptr = (get_slab_capacity(sizeclass(), is_short) * size) + offset;
      // Check we haven't allocated more than fits in a slab
      SNMALLOC_ASSERT(bumpptr <= SLAB_SIZE);

      // Account for to be bump allocated space
      accounted_for += SLAB_SIZE - bumpptr;

      SNMALLOC_ASSERT(!is_sleeping());

      // All space accounted for
      SNMALLOC_ASSERT(SLAB_SIZE == accounted_for);
#else
      UNUSED(slab);
      UNUSED(entropy);
#endif
    }
  };

  struct RemoteAllocator;

  /**
   * Entry stored in the pagemap.
   */
  class MetaEntry
  {
    Metaslab* meta;
    uintptr_t remote_and_sizeclass;

  public:
    constexpr MetaEntry() : meta(nullptr), remote_and_sizeclass(0) {}

    MetaEntry(Metaslab* meta, RemoteAllocator* remote, sizeclass_t sizeclass)
    : meta(meta)
    {
      remote_and_sizeclass =
        address_cast(pointer_offset<char>(remote, sizeclass));
    }

    Metaslab* get_metaslab() const
    {
      return meta;
    }

    RemoteAllocator* get_remote() const
    {
      return reinterpret_cast<RemoteAllocator*>(
        bits::align_down(remote_and_sizeclass, alignof(RemoteAllocator)));
    }

    sizeclass_t get_sizeclass() const
    {
      return remote_and_sizeclass & (alignof(RemoteAllocator) - 1);
    }
  };

  struct MetaslabCache : public CDLLNode<>
  {
    uint16_t unused;
    uint16_t length;
  };

} // namespace snmalloc
