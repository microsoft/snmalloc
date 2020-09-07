#pragma once

#include "superslab.h"

namespace snmalloc
{
  struct FreeListHead
  {
    // Use a value with bottom bit set for empty list.
    void* value = nullptr;
  };

  class Slab
  {
  private:
    uint16_t pointer_to_index(void* p)
    {
      // Get the offset from the slab for a memory location.
      return static_cast<uint16_t>(pointer_diff(this, p));
    }

  public:
    Metaslab& get_meta()
    {
      Superslab* super = Superslab::get(this);
      return super->get_meta(this);
    }

    SlabLink* get_link()
    {
      return get_meta().get_link(this);
    }

    /**
     * Takes a free list out of a slabs meta data.
     * Returns the link as the allocation, and places the free list into the
     * `fast_free_list` for further allocations.
     */
    template<ZeroMem zero_mem, SNMALLOC_CONCEPT(ConceptPAL) PAL>
    SNMALLOC_FAST_PATH void*
    alloc(SlabList& sl, FreeListHead& fast_free_list, size_t rsize)
    {
      // Read the head from the metadata stored in the superslab.
      Metaslab& meta = get_meta();
      SNMALLOC_ASSERT(meta.link != 1);

      SNMALLOC_ASSERT(rsize == sizeclass_to_size(meta.sizeclass));
      SNMALLOC_ASSERT(
        sl.get_next() == (SlabLink*)pointer_offset(this, meta.link));
      SNMALLOC_ASSERT(!meta.is_full());
      meta.debug_slab_invariant(this);

      // Put everything in allocators small_class free list.
      fast_free_list.value = meta.head;
      meta.head = nullptr;

      // Return the link as the node for this allocation.
      void* link = pointer_offset(this, meta.link);
      void* p = remove_cache_friendly_offset(link, meta.sizeclass);

      // Treat stealing the free list as allocating it all.
      meta.needed = meta.allocated;
      meta.set_full();
      sl.get_next()->remove();

      SNMALLOC_ASSERT(is_start_of_object(Superslab::get(p), p));

      meta.debug_slab_invariant(this);

      if constexpr (zero_mem == YesZero)
      {
        if (rsize < PAGE_ALIGNED_SIZE)
          PAL::zero(p, rsize);
        else
          PAL::template zero<true>(p, rsize);
      }
      else
      {
        UNUSED(rsize);
      }

      return p;
    }

    /**
     * Given a bumpptr and a fast_free_list head reference, builds a new free
     * list, and stores it in the fast_free_list. It will only create a page
     * worth of allocations, or one if the allocation size is larger than a
     * page.
     */
    static SNMALLOC_FAST_PATH void
    alloc_new_list(void*& bumpptr, FreeListHead& fast_free_list, size_t rsize)
    {
      fast_free_list.value = bumpptr;
      void* newbumpptr = pointer_offset(bumpptr, rsize);
      void* slab_end = pointer_align_up<SLAB_SIZE>(newbumpptr);
      void* slab_end2 =
        pointer_align_up<OS_PAGE_SIZE>(pointer_offset(bumpptr, rsize * 32));
      if (slab_end2 < slab_end)
        slab_end = slab_end2;

      while (newbumpptr < slab_end)
      {
        Metaslab::store_next(bumpptr, newbumpptr);
        bumpptr = newbumpptr;
        newbumpptr = pointer_offset(bumpptr, rsize);
      }

      Metaslab::store_next(bumpptr, nullptr);
      bumpptr = newbumpptr;
    }

    bool is_start_of_object(Superslab* super, void* p)
    {
      Metaslab& meta = super->get_meta(this);
      return is_multiple_of_sizeclass(
        sizeclass_to_size(meta.sizeclass),
        pointer_diff(p, pointer_offset(this, SLAB_SIZE)));
    }

    // Returns true, if it deallocation can proceed without changing any status
    // bits. Note that this does remove the use from the meta slab, so it
    // doesn't need doing on the slow path.
    SNMALLOC_FAST_PATH bool dealloc_fast(Superslab* super, void* p)
    {
      Metaslab& meta = super->get_meta(this);
#ifdef CHECK_CLIENT
      if (meta.is_unused())
        error("Detected potential double free.");
#endif

      if (unlikely(meta.return_object()))
        return false;

      // Update the head and the next pointer in the free list.
      void* head = meta.head;

      // Set the head to the memory being deallocated.
      meta.head = p;
      SNMALLOC_ASSERT(meta.valid_head());

      // Set the next pointer to the previous head.
      Metaslab::store_next(p, head);

      return true;
    }

    // If dealloc fast returns false, then call this.
    // This does not need to remove the "use" as done by the fast path.
    // Returns a complex return code for managing the superslab meta data.
    // i.e. This deallocation could make an entire superslab free.
    SNMALLOC_SLOW_PATH typename Superslab::Action
    dealloc_slow(SlabList* sl, Superslab* super, void* p)
    {
      Metaslab& meta = super->get_meta(this);
      meta.debug_slab_invariant(this);

      if (meta.is_full())
      {
        // We are not on the sizeclass list.
        if (meta.allocated == 1)
        {
          // Dealloc on the superslab.
          if (is_short())
            return super->dealloc_short_slab();

          return super->dealloc_slab(this);
        }
        // Update the head and the sizeclass link.
        uint16_t index = pointer_to_index(p);
        SNMALLOC_ASSERT(meta.head == nullptr);
        //        SNMALLOC_ASSERT(meta.fully_allocated(is_short()));
        meta.link = index;
        meta.needed = meta.allocated - 1;

        // Push on the list of slabs for this sizeclass.
        sl->insert_prev(meta.get_link(this));
        meta.debug_slab_invariant(this);
        return Superslab::NoSlabReturn;
      }

      // Remove from the sizeclass list and dealloc on the superslab.
      meta.get_link(this)->remove();

      if (is_short())
        return super->dealloc_short_slab();

      return super->dealloc_slab(this);
    }

    bool is_short()
    {
      return Metaslab::is_short(this);
    }
  };
} // namespace snmalloc
