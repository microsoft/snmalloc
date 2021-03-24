#pragma once

#include "freelist.h"
#include "superslab.h"

#include <array>

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
      void* slab_end = pointer_align_up<SLAB_SIZE>(pointer_offset(bumpptr, 1));

      FreeListBuilder b;
      SNMALLOC_ASSERT(b.empty());

      b.open(bumpptr);

      // This code needs generalising, but currently applies
      // various offsets with a stride of seven to increase chance of catching
      // accidental OOB write.
      std::array<size_t, 7> start_index = {3, 5, 0, 2, 4, 1, 6};
      for (size_t offset : start_index)
      {
        void* newbumpptr = pointer_offset(bumpptr, rsize * offset);
        while (newbumpptr < slab_end)
        {
          b.add(newbumpptr);
          newbumpptr = pointer_offset(newbumpptr, rsize * start_index.size());
        }
      }
      bumpptr = slab_end;

      SNMALLOC_ASSERT(!b.empty());
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
      SNMALLOC_ASSERT(!meta.is_unused());

      if (unlikely(meta.return_object()))
        return false;

      // Update the head and the next pointer in the free list.
      meta.free_queue.add(p);

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
        auto allocated = get_slab_capacity(
          meta.sizeclass(), Metaslab::is_short(Metaslab::get_slab(p)));
        // We are not on the sizeclass list.
        if (allocated == 1)
        {
          // Dealloc on the superslab.
          if (Metaslab::is_short(self))
            return super->dealloc_short_slab();

          return super->dealloc_slab(self);
        }
        SNMALLOC_ASSERT(meta.free_queue.empty());
        meta.free_queue.open(p);
        meta.free_queue.add(p);
        meta.needed() = allocated - 1;

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
