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

    template<ZeroMem zero_mem, typename MemoryProvider>
    inline void* alloc(
      SlabList& sl,
      FreeListHead& fast_free_list,
      size_t rsize,
      MemoryProvider& memory_provider)
    {
      // Read the head from the metadata stored in the superslab.
      Metaslab& meta = get_meta();
      void* head = meta.head;

      SNMALLOC_ASSERT(rsize == sizeclass_to_size(meta.sizeclass));
      SNMALLOC_ASSERT(
        sl.get_head() == (SlabLink*)pointer_offset(this, meta.link));
      SNMALLOC_ASSERT(!meta.is_full());
      meta.debug_slab_invariant(this);

      void* p = nullptr;
      bool p_has_value = false;

      if (head == nullptr)
      {
        size_t bumpptr = get_initial_offset(meta.sizeclass, is_short());
        bumpptr += meta.allocated * rsize;
        if (bumpptr == SLAB_SIZE)
        {
          // Everything is in use, so we need all entries to be
          // return before we can reclaim this slab.
          meta.needed = meta.allocated;

          void* link = pointer_offset(this, meta.link);
          p = remove_cache_friendly_offset(link, meta.sizeclass);

          meta.set_full();
          sl.pop();
          p_has_value = true;
        }
        else
        {
          // Allocate the last object on the current page if there is one,
          // and then thread the next free list worth of allocations.
          bool crossed_page_boundary = false;
          void* curr = nullptr;
          while (true)
          {
            size_t newbumpptr = bumpptr + rsize;
            auto alignedbumpptr = bits::align_up(bumpptr - 1, OS_PAGE_SIZE);
            auto alignednewbumpptr = bits::align_up(newbumpptr, OS_PAGE_SIZE);

            if (alignedbumpptr != alignednewbumpptr)
            {
              // We have crossed a page boundary already, so
              // lets stop building our free list.
              if (crossed_page_boundary)
                break;

              crossed_page_boundary = true;
            }

            if (curr == nullptr)
            {
              meta.head = pointer_offset(this, bumpptr);
            }
            else
            {
              Metaslab::store_next(
                curr, (bumpptr == 1) ? nullptr : pointer_offset(this, bumpptr));
            }
            curr = pointer_offset(this, bumpptr);
            bumpptr = newbumpptr;
            meta.allocated = meta.allocated + 1;
          }

          SNMALLOC_ASSERT(curr != nullptr);
          Metaslab::store_next(curr, nullptr);
        }
      }

      if (!p_has_value)
      {
        p = meta.head;

        // Read the next slot from the memory that's about to be allocated.
        void* next = Metaslab::follow_next(p);
        // Put everything in allocators small_class free list.
        meta.head = nullptr;
        fast_free_list.value = next;
        // Treat stealing the free list as allocating it all.
        // Link is not in use, i.e. - 1 is required.
        meta.needed = meta.allocated - 1;

        p = remove_cache_friendly_offset(p, meta.sizeclass);
      }

      SNMALLOC_ASSERT(is_start_of_object(Superslab::get(p), p));

      meta.debug_slab_invariant(this);

      if constexpr (zero_mem == YesZero)
      {
        if (rsize < PAGE_ALIGNED_SIZE)
          memory_provider.zero(p, rsize);
        else
          memory_provider.template zero<true>(p, rsize);
      }

      return p;
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
        sl->insert_back(meta.get_link(this));
        meta.debug_slab_invariant(this);
        return Superslab::NoSlabReturn;
      }

      // Remove from the sizeclass list and dealloc on the superslab.
      sl->remove(meta.get_link(this));

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
