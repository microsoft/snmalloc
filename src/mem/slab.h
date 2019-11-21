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
      return static_cast<uint16_t>(address_cast(p) - address_cast(this));
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

      assert(rsize == sizeclass_to_size(meta.sizeclass));
      meta.debug_slab_invariant(this);
      assert(sl.get_head() == (SlabLink*)((size_t)this + meta.link));
      assert(!meta.is_full());

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
          void* curr = nullptr;
          bool commit = false;
          while (true)
          {
            size_t newbumpptr = bumpptr + rsize;
            auto alignedbumpptr = bits::align_up(bumpptr - 1, OS_PAGE_SIZE);
            auto alignednewbumpptr = bits::align_up(newbumpptr, OS_PAGE_SIZE);

            if (alignedbumpptr != alignednewbumpptr)
            {
              // We have committed once already.
              if (commit)
                break;

              memory_provider.template notify_using<NoZero>(
                pointer_offset(this, alignedbumpptr),
                alignednewbumpptr - alignedbumpptr);

              commit = true;
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

          assert(curr != nullptr);
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

      assert(is_start_of_object(Superslab::get(p), p));

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
        address_cast(this) + SLAB_SIZE - address_cast(p));
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
      meta.debug_slab_invariant(this);

      if (unlikely(meta.return_object()))
        return false;

      // Update the head and the next pointer in the free list.
      void* head = meta.head;

      // Set the head to the memory being deallocated.
      meta.head = p;
      assert(meta.valid_head());

      // Set the next pointer to the previous head.
      Metaslab::store_next(p, head);
      meta.debug_slab_invariant(this);
      return true;
    }

    // If dealloc fast returns false, then call this.
    // This does not need to remove the "use" as done by the fast path.
    // Returns a complex return code for managing the superslab meta data.
    // i.e. This deallocation could make an entire superslab free.
    template<typename MemoryProvider>
    SNMALLOC_SLOW_PATH typename Superslab::Action dealloc_slow(
      SlabList* sl, Superslab* super, void* p, MemoryProvider& memory_provider)
    {
      Metaslab& meta = super->get_meta(this);

      if (meta.is_full())
      {
        // We are not on the sizeclass list.
        if (meta.allocated == 1)
        {
          // Dealloc on the superslab.
          if (is_short())
            return super->dealloc_short_slab(memory_provider);

          return super->dealloc_slab(this, memory_provider);
        }
        // Update the head and the sizeclass link.
        uint16_t index = pointer_to_index(p);
        assert(meta.head == nullptr);
        //        assert(meta.fully_allocated(is_short()));
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
        return super->dealloc_short_slab(memory_provider);

      return super->dealloc_slab(this, memory_provider);
    }
    bool is_short()
    {
      return Metaslab::is_short(this);
    }
  };
} // namespace snmalloc
