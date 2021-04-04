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
    static SNMALLOC_FAST_PATH void alloc_new_list(
      void*& bumpptr,
      FreeListIter& fast_free_list,
      size_t rsize,
      LocalEntropy& entropy)
    {
      void* slab_end = pointer_align_up<SLAB_SIZE>(pointer_offset(bumpptr, 1));

      FreeListBuilder<false> b;
      SNMALLOC_ASSERT(b.empty());

      b.open(bumpptr);

#ifdef CHECK_CLIENT
      // Structure to represent the temporary list elements
      struct PreAllocObject
      {
        PreAllocObject* next;
      };
      // The following code implements Sattolo's algorithm for generating
      // random cyclic permutations.  This implementation is in the opposite
      // direction, so that the original space does not need initialising.  This
      // is described as outside-in without citation on Wikipedia, appears to be
      // Folklore algorithm.
      PreAllocObject* curr = pointer_offset<PreAllocObject>(bumpptr, 0);
      curr->next = curr;
      uint16_t count = 1;
      for (PreAllocObject* p = pointer_offset<PreAllocObject>(bumpptr, rsize);
           p < slab_end;
           p = pointer_offset<PreAllocObject>(p, rsize))
      {
        size_t insert_index = entropy.sample(count);
        p->next = std::exchange(
          pointer_offset<PreAllocObject>(bumpptr, insert_index * rsize)->next,
          p);
        count++;
      }

      // Pick entry into space, and then build linked list by traversing cycle
      // to the start.
      auto start_index = entropy.sample(count);
      auto start_ptr =
        pointer_offset<PreAllocObject>(bumpptr, start_index * rsize);
      auto curr_ptr = start_ptr;
      do
      {
        b.add(curr_ptr, entropy);
        curr_ptr = curr_ptr->next;
      } while (curr_ptr != start_ptr);
#else
      for (void* p = bumpptr; p < slab_end; p = pointer_offset<void>(p, rsize))
      {
        b.add(p, entropy);
      }
#endif
      // This code consumes everything up to slab_end.
      bumpptr = slab_end;

      SNMALLOC_ASSERT(!b.empty());
      b.close(fast_free_list, entropy);
    }

    // Returns true, if it deallocation can proceed without changing any status
    // bits. Note that this does remove the use from the meta slab, so it
    // doesn't need doing on the slow path.
    //
    // This is pre-factored to take an explicit self parameter so that we can
    // eventually annotate that pointer with additional information.
    static SNMALLOC_FAST_PATH bool
    dealloc_fast(Slab* self, Superslab* super, void* p, LocalEntropy& entropy)
    {
      Metaslab& meta = super->get_meta(self);
      SNMALLOC_ASSERT(!meta.is_unused());

      if (unlikely(meta.return_object()))
        return false;

      // Update the head and the next pointer in the free list.
      meta.free_queue.add(p, entropy);

      return true;
    }

    // If dealloc fast returns false, then call this.
    // This does not need to remove the "use" as done by the fast path.
    // Returns a complex return code for managing the superslab meta data.
    // i.e. This deallocation could make an entire superslab free.
    //
    // This is pre-factored to take an explicit self parameter so that we can
    // eventually annotate that pointer with additional information.
    static SNMALLOC_SLOW_PATH typename Superslab::Action dealloc_slow(
      Slab* self,
      SlabList* sl,
      Superslab* super,
      void* p,
      LocalEntropy& entropy)
    {
      Metaslab& meta = super->get_meta(self);
      meta.debug_slab_invariant(self, entropy);

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

        meta.free_queue.add(p, entropy);
        //  Remove trigger threshold from how many we need before we have fully
        //  freed the slab.
        meta.needed() =
          allocated - meta.threshold_for_waking_slab(Metaslab::is_short(self));

        // Push on the list of slabs for this sizeclass.
        sl->insert_prev(&meta);
        meta.debug_slab_invariant(self, entropy);
        return Superslab::NoSlabReturn;
      }

#ifdef CHECK_CLIENT
      size_t count = 1;
      // Check free list is well-formed on platforms with
      // integers as pointers.
      FreeListIter fl;
      meta.free_queue.close(fl, entropy);

      while (!fl.empty())
      {
        fl.take(entropy);
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
