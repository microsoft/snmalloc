#pragma once

#include "../ds/cdllist.h"
#include "../ds/dllist.h"
#include "../ds/helpers.h"
#include "freelist.h"
#include "ptrhelpers.h"
#include "sizeclass.h"

namespace snmalloc
{
  class Slab;

  using SlabList = CDLLNode<>;
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
     *  How many entries are not in the free list of slab, i.e.
     *  how many entries are needed to fully free this slab.
     *
     *  In the case of a fully allocated slab, where prev==0 needed
     *  will be 1. This enables 'return_object' to detect the slow path
     *  case with a single operation subtract and test.
     */
    uint16_t needed = 0;

    // TODO something for the nullptr sizeclass
    uint8_t sizeclass = 255;
    // Initially zero to encode the superslabs relative list of slabs.
    uint8_t next = 0;
  };

  static constexpr size_t MIN_OBJECT_COUNT = 13;

  inline static size_t sizeclass_to_slab_size(sizeclass_t sizeclass)
  {
    size_t rsize = sizeclass_to_size(sizeclass);
    size_t slab_bits =
      bits::max(bits::next_pow2_bits(MIN_OBJECT_COUNT * rsize), MIN_CHUNK_BITS);
    return bits::one_at_bit(slab_bits);
  }

  inline static size_t sizeclass_to_slab_sizeclass(sizeclass_t sizeclass)
  {
    size_t ssize = sizeclass_to_slab_size(sizeclass);

    return bits::next_pow2_bits(ssize) - MIN_CHUNK_BITS;
  }

  inline static size_t slab_sizeclass_to_size(sizeclass_t sizeclass)
  {
    return bits::one_at_bit(MIN_CHUNK_BITS + sizeclass);
  }

  inline static uint16_t sizeclass_to_slab_object_count(sizeclass_t sizeclass)
  {
    size_t rsize = sizeclass_to_size(sizeclass);

    return (uint16_t)(sizeclass_to_slab_size(sizeclass) / rsize);
  }

  inline static size_t large_size_to_slab_size(size_t size)
  {
    return bits::next_pow2(size);
  }

  inline static size_t large_size_to_slab_sizeclass(size_t size)
  {
    return bits::next_pow2_bits(size) - MIN_CHUNK_BITS;
  }

  // The Metaslab represent the status of a single slab.
  // This can be either a short or a standard slab.
  class Metaslab : public SlabLink
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

    uint8_t sizeclass()
    {
      return free_queue.s.sizeclass;
    }

    uint8_t& next()
    {
      return free_queue.s.next;
    }

    void initialise(sizeclass_t sizeclass)
    {
      // TODO: Special version for large alloc?
      //    TODO: Assert this is a Large alloc
      //    TODO: other fields for other data?

      free_queue.s.sizeclass = static_cast<uint8_t>(sizeclass);
      free_queue.init();
      // Set up meta data as if the entire slab has been turned into a free
      // list. This means we don't have to check for special cases where we have
      // returned all the elements, but this is a slab that is still being bump
      // allocated from. Hence, the bump allocator slab will never be returned
      // for use in another size class.
      set_full();
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

    bool is_full()
    {
      return get_prev() == nullptr;
    }

    /**
     * Only wake slab if we have this many free allocations
     *
     * This helps remove bouncing around empty to non-empty cases.
     *
     * It also increases entropy, when we have randomisation.
     */
    uint16_t threshold_for_waking_slab()
    {
      auto capacity = sizeclass_to_slab_object_count(sizeclass());
      uint16_t threshold = (capacity / 16) + 3;
      uint16_t max = 32;
      return bits::min(threshold, max);
    }

    SNMALLOC_FAST_PATH void set_full()
    {
      SNMALLOC_ASSERT(free_queue.empty());

      // Prepare for the next free queue to be built.
      free_queue.open();

      // Set needed to at least one, possibly more so we only use
      // a slab when it has a reasonable amount of free elements
      needed() = threshold_for_waking_slab();
      null_prev();
    }

    template<typename T, capptr_bounds B>
    static SNMALLOC_FAST_PATH CapPtr<Slab, capptr_bound_chunkd_bounds<B>()>
    get_slab(CapPtr<T, B> p)
    {
      static_assert(B == CBArena || B == CBChunkD || B == CBChunk);

      return capptr_bound_chunkd(
        pointer_align_down<SLAB_SIZE, Slab>(p.as_void()), SLAB_SIZE);
    }

    template<capptr_bounds B>
    static bool is_short(CapPtr<Slab, B> p)
    {
      return pointer_align_down<SUPERSLAB_SIZE, Slab>(p.as_void()) == p;
    }

    SNMALLOC_FAST_PATH bool is_start_of_object(address_t p)
    {
      return is_multiple_of_sizeclass(
        sizeclass(),
        p - (bits::align_down(p, sizeclass_to_slab_size(sizeclass()))));
    }

    /**
     * Takes a free list out of a slabs meta data.
     * Returns the link as the allocation, and places the free list into the
     * `fast_free_list` for further allocations.
     */
    static SNMALLOC_FAST_PATH CapPtr<void, CBAllocE>
    alloc(Metaslab* meta, FreeListIter& fast_free_list, LocalEntropy& entropy)
    {
      meta->free_queue.close(fast_free_list, entropy);
      auto p = fast_free_list.take(entropy);

      entropy.refresh_bits();

      // Treat stealing the free list as allocating it all.
      meta->remove();
      meta->set_full();

      SNMALLOC_ASSERT(meta->is_start_of_object(address_cast(p)));

      //    TODO?
      //      self->debug_slab_invariant(meta, entropy);

      // TODO: Should this be zeroing the FreeObject state?
      return capptr_export(p.as_void());
    }

    template<capptr_bounds B>
    void debug_slab_invariant(CapPtr<Slab, B> slab, LocalEntropy& entropy)
    {
      static_assert(B == CBChunkD || B == CBChunk);

#if !defined(NDEBUG) && !defined(SNMALLOC_CHEAP_CHECKS)
      bool is_short = Metaslab::is_short(slab);

      if (is_full())
      {
        size_t count = free_queue.debug_length(entropy);
        SNMALLOC_ASSERT(count < threshold_for_waking_slab());
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

      SNMALLOC_ASSERT(!is_full());

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
  struct MetaEntry
  {
    Metaslab* meta;
    RemoteAllocator* remote;

    constexpr MetaEntry(Metaslab* meta, RemoteAllocator* remote)
    : meta(meta), remote(remote)
    {}
  };

} // namespace snmalloc
