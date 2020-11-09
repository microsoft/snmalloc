#pragma once

#include "../ds/helpers.h"
#include "allocslab.h"
#include "metaslab.h"

#include <new>

namespace snmalloc
{
  class Superslab : public Allocslab
  {
    // This is the view of a 16 mb superslab when it is being used to allocate
    // 64 kb slabs.
  private:
    friend DLList<Superslab>;

    // Keep the allocator pointer on a separate cache line. It is read by
    // other threads, and does not change, so we avoid false sharing.
    alignas(CACHELINE_SIZE)
      // The superslab is kept on a doubly linked list of superslabs which
      // have some space.
      Superslab* next;
    Superslab* prev;

    // This is a reference to the first unused slab in the free slab list
    // It is does not contain the short slab, which is handled using a bit
    // in the "used" field below.  The list is terminated by pointing to
    // the short slab.
    // The head linked list has an absolute pointer for head, but the next
    // pointers stores in the metaslabs are relative pointers, that is they
    // are the relative offset to the next entry minus 1.  This means that
    // all zeros is a list that chains through all the blocks, so the zero
    // initialised memory requires no more work.
    Mod<SLAB_COUNT, uint8_t> head;

    // Represents twice the number of full size slabs used
    // plus 1 for the short slab. i.e. using 3 slabs and the
    // short slab would be 6 + 1 = 7
    uint16_t used;

    ModArray<SLAB_COUNT, Metaslab> meta;

    // Used size_t as results in better code in MSVC
    size_t slab_to_index(Slab* slab)
    {
      auto res = (pointer_diff(this, slab) >> SLAB_BITS);
      SNMALLOC_ASSERT(res == static_cast<uint8_t>(res));
      return static_cast<uint8_t>(res);
    }

  public:
    enum Status
    {
      Full,
      Available,
      OnlyShortSlabAvailable,
      Empty
    };

    enum Action
    {
      NoSlabReturn = 0,
      NoStatusChange = 1,
      StatusChange = 2
    };

    static Superslab* get(const void* p)
    {
      return pointer_align_down<SUPERSLAB_SIZE, Superslab>(
        const_cast<void*>(p));
    }

    static bool is_short_sizeclass(sizeclass_t sizeclass)
    {
      constexpr sizeclass_t h = size_to_sizeclass_const(sizeof(Superslab));
      return sizeclass <= h;
    }

    void init(RemoteAllocator* alloc)
    {
      allocator = alloc;

      // If Superslab is larger than a page, then we cannot guarantee it still
      // has a valid layout as the subsequent pages could have been freed and
      // zeroed, hence only skip initialisation if smaller.
      if (kind != Super || (sizeof(Superslab) >= OS_PAGE_SIZE))
      {
        if (kind != Fresh)
        {
          // If this wasn't previously Fresh, we need to zero some things.
          used = 0;
          for (size_t i = 0; i < SLAB_COUNT; i++)
          {
            new (&(meta[i])) Metaslab();
          }
        }

        // If this wasn't previously a Superslab, we need to set up the
        // header.
        kind = Super;
        // Point head at the first non-short slab.
        head = 1;
      }

#ifndef NDEBUG
      auto curr = head;
      for (size_t i = 0; i < SLAB_COUNT - used - 1; i++)
      {
        curr = (curr + meta[curr].next + 1) & (SLAB_COUNT - 1);
      }
      if (curr != 0)
        abort();

      for (size_t i = 0; i < SLAB_COUNT; i++)
      {
        SNMALLOC_ASSERT(meta[i].is_unused());
      }
#endif
    }

    bool is_empty()
    {
      return used == 0;
    }

    bool is_full()
    {
      return (used == (((SLAB_COUNT - 1) << 1) + 1));
    }

    bool is_almost_full()
    {
      return (used >= ((SLAB_COUNT - 1) << 1));
    }

    Status get_status()
    {
      if (!is_almost_full())
      {
        if (!is_empty())
        {
          return Available;
        }

        return Empty;
      }

      if (!is_full())
      {
        return OnlyShortSlabAvailable;
      }

      return Full;
    }

    Metaslab& get_meta(Slab* slab)
    {
      return meta[slab_to_index(slab)];
    }

    Slab* alloc_short_slab(sizeclass_t sizeclass)
    {
      if ((used & 1) == 1)
        return alloc_slab(sizeclass);

      meta[0].head = nullptr;
      // Set up meta data as if the entire slab has been turned into a free
      // list. This means we don't have to check for special cases where we have
      // returned all the elements, but this is a slab that is still being bump
      // allocated from. Hence, the bump allocator slab will never be returned
      // for use in another size class.
      meta[0].allocated = static_cast<uint16_t>(
        (SLAB_SIZE - get_initial_offset(sizeclass, true)) /
        sizeclass_to_size(sizeclass));
      meta[0].link = 1;
      meta[0].needed = 1;
      meta[0].sizeclass = static_cast<uint8_t>(sizeclass);

      used++;
      return reinterpret_cast<Slab*>(this);
    }

    Slab* alloc_slab(sizeclass_t sizeclass)
    {
      uint8_t h = head;
      Slab* slab = pointer_offset(
        reinterpret_cast<Slab*>(this), (static_cast<size_t>(h) << SLAB_BITS));

      uint8_t n = meta[h].next;

      meta[h].head = nullptr;
      // Set up meta data as if the entire slab has been turned into a free
      // list. This means we don't have to check for special cases where we have
      // returned all the elements, but this is a slab that is still being bump
      // allocated from. Hence, the bump allocator slab will never be returned
      // for use in another size class.
      meta[h].allocated = static_cast<uint16_t>(
        (SLAB_SIZE - get_initial_offset(sizeclass, false)) /
        sizeclass_to_size(sizeclass));
      meta[h].needed = 1;
      meta[h].link = 1;
      meta[h].sizeclass = static_cast<uint8_t>(sizeclass);

      head = h + n + 1;
      used += 2;

      return slab;
    }

    // Returns true, if this alters the value of get_status
    Action dealloc_slab(Slab* slab)
    {
      // This is not the short slab.
      uint8_t index = static_cast<uint8_t>(slab_to_index(slab));
      uint8_t n = head - index - 1;

      meta[index].sizeclass = 0;
      meta[index].next = n;
      head = index;
      bool was_almost_full = is_almost_full();
      used -= 2;

      SNMALLOC_ASSERT(meta[index].is_unused());
      if (was_almost_full || is_empty())
        return StatusChange;

      return NoStatusChange;
    }

    // Returns true, if this alters the value of get_status
    Action dealloc_short_slab()
    {
      bool was_full = is_full();
      used--;

      SNMALLOC_ASSERT(meta[0].is_unused());
      if (was_full || is_empty())
        return StatusChange;

      return NoStatusChange;
    }
  };
} // namespace snmalloc
