#pragma once

#include "freelist.h"
#include "superslab.h"

namespace snmalloc
{
  class Slab
  {
  private:
    uint16_t address_to_index(address_t p)
    {
      // Get the offset from the slab for a memory location.
      return static_cast<uint16_t>(p - address_cast(this));
    }

  public:
    static Metaslab& get_meta(Slab* self)
    {
      Superslab* super = Superslab::get(self);
      return super->get_meta(self);
    }

    /**
     * Given a bumpptr and a fast_free_list head reference, builds a new free
     * list, and stores it in the fast_free_list. It will only create a page
     * worth of allocations, or one if the allocation size is larger than a
     * page.
     */
    static SNMALLOC_FAST_PATH void
    alloc_new_list(void*& bumpptr, FreeListIter& fast_free_list, size_t rsize)
    {
      FreeListBuilder b;
      b.open(bumpptr);

      void* newbumpptr = pointer_offset(bumpptr, rsize);
      void* slab_end = pointer_align_up<SLAB_SIZE>(newbumpptr);
      void* slab_end2 =
        pointer_align_up<OS_PAGE_SIZE>(pointer_offset(bumpptr, rsize * 32));
      if (slab_end2 < slab_end)
        slab_end = slab_end2;

      bumpptr = newbumpptr;
      while (bumpptr < slab_end)
      {
        b.add(bumpptr);
        bumpptr = pointer_offset(bumpptr, rsize);
      }

      b.close(fast_free_list);
    }

    // Returns true, if it deallocation can proceed without changing any status
    // bits. Note that this does remove the use from the meta slab, so it
    // doesn't need doing on the slow path.
    //
    // This is pre-factored to take an explicit self parameter so that we can
    // eventually annotate that pointer with additional information.
    static SNMALLOC_FAST_PATH bool
    dealloc_fast(Slab* self, Superslab* super, void* p)
    {
      Metaslab& meta = super->get_meta(self);
#ifdef CHECK_CLIENT
      if (meta.is_unused())
        error("Detected potential double free.");
#endif

      if (unlikely(meta.return_object()))
        return false;

      // Update the head and the next pointer in the free list.
      meta.free_queue.add(p);

      SNMALLOC_ASSERT(meta.valid_head());

      return true;
    }

    // If dealloc fast returns false, then call this.
    // This does not need to remove the "use" as done by the fast path.
    // Returns a complex return code for managing the superslab meta data.
    // i.e. This deallocation could make an entire superslab free.
    //
    // This is pre-factored to take an explicit self parameter so that we can
    // eventually annotate that pointer with additional information.
    static SNMALLOC_SLOW_PATH typename Superslab::Action
    dealloc_slow(Slab* self, SlabList* sl, Superslab* super, void* p)
    {
      Metaslab& meta = super->get_meta(self);
      meta.debug_slab_invariant(self);

      if (meta.is_full())
      {
        // We are not on the sizeclass list.
        if (meta.allocated == 1)
        {
          // Dealloc on the superslab.
          if (Metaslab::is_short(self))
            return super->dealloc_short_slab();

          return super->dealloc_slab(self);
        }
        SNMALLOC_ASSERT(meta.free_queue.empty());
        meta.free_queue.open(p);
        meta.needed = meta.allocated - 1;

        // Push on the list of slabs for this sizeclass.
        sl->insert_prev(&meta);
        meta.debug_slab_invariant(self);
        return Superslab::NoSlabReturn;
      }

#ifdef CHECK_CLIENT
      size_t count = 1;
      // Check free list is well-formed on platforms with
      // integers as pointers.
      FreeListIter fl;
      meta.free_queue.close(fl);

      while (!fl.empty())
      {
        fl.take();
        count++;
      }
#endif

      meta.remove();

      if (Metaslab::is_short(self))
        return super->dealloc_short_slab();
      return super->dealloc_slab(self);
    }
  };
} // namespace snmalloc
