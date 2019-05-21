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
      auto res = ((address_cast(slab) - address_cast(this)) >> SLAB_BITS);
      assert(res == (uint8_t)res);
      return res;
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

    static Superslab* get(void* p)
    {
      return pointer_cast<Superslab>(address_cast(p) & SUPERSLAB_MASK);
    }

    static bool is_short_sizeclass(uint8_t sizeclass)
    {
      constexpr uint8_t h = size_to_sizeclass_const(sizeof(Superslab));
      return sizeclass <= h;
    }

    void init(RemoteAllocator* alloc)
    {
      allocator = alloc;

      if (kind != Super)
      {
        // If this wasn't previously a Superslab, we need to set up the
        // header.
        kind = Super;
        // Point head at the first non-short slab.
        head = 1;

        if (kind != Fresh)
        {
          // If this wasn't previously Fresh, we need to zero some things.
          used = 0;
          for (size_t i = 0; i < SLAB_COUNT; i++)
          {
            new (&(meta[i])) Metaslab();
          }
        }
#ifndef NDEBUG
        auto curr = head;
        for (size_t i = 0; i < SLAB_COUNT - used - 1; i++)
        {
          curr = (curr + meta[curr].next + 1) & (SLAB_COUNT - 1);
        }
        assert(curr == 0);

        for (size_t i = 0; i < SLAB_COUNT; i++)
        {
          assert(meta[i].is_unused());
        }
#endif
      }
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

    template<typename MemoryProvider>
    Slab* alloc_short_slab(uint8_t sizeclass, MemoryProvider& memory_provider)
    {
      if ((used & 1) == 1)
        return alloc_slab(sizeclass, memory_provider);

      meta[0].head = get_initial_bumpptr(sizeclass, true);
      meta[0].sizeclass = sizeclass;
      meta[0].link = get_initial_link(sizeclass, true);

      if constexpr (decommit_strategy == DecommitAll)
      {
        memory_provider.template notify_using<NoZero>(
          pointer_offset(this, OS_PAGE_SIZE), SLAB_SIZE - OS_PAGE_SIZE);
      }

      used++;
      return (Slab*)this;
    }

    template<typename MemoryProvider>
    Slab* alloc_slab(uint8_t sizeclass, MemoryProvider& memory_provider)
    {
      uint8_t h = head;
      Slab* slab = pointer_cast<Slab>(
        address_cast(this) + (static_cast<size_t>(h) << SLAB_BITS));

      uint8_t n = meta[h].next;

      meta[h].head = get_initial_bumpptr(sizeclass, false);
      meta[h].sizeclass = sizeclass;
      meta[h].link = get_initial_link(sizeclass, false);

      head = h + n + 1;
      used += 2;

      if constexpr (decommit_strategy == DecommitAll)
      {
        memory_provider.template notify_using<NoZero>(slab, SLAB_SIZE);
      }

      return slab;
    }

    // Returns true, if this alters the value of get_status
    template<typename MemoryProvider>
    Action dealloc_slab(Slab* slab, MemoryProvider& memory_provider)
    {
      // This is not the short slab.
      uint8_t index = static_cast<uint8_t>(slab_to_index(slab));
      uint8_t n = head - index - 1;

      meta[index].sizeclass = 0;
      meta[index].next = n;
      head = index;
      bool was_almost_full = is_almost_full();
      used -= 2;

      if constexpr (decommit_strategy == DecommitAll)
        memory_provider.notify_not_using(slab, SLAB_SIZE);

      assert(meta[index].is_unused());
      if (was_almost_full || is_empty())
        return StatusChange;

      return NoStatusChange;
    }

    // Returns true, if this alters the value of get_status
    template<typename MemoryProvider>
    Action dealloc_short_slab(MemoryProvider& memory_provider)
    {
      // This is the short slab.
      if constexpr (decommit_strategy == DecommitAll)
      {
        memory_provider.notify_not_using(
          pointer_offset(this, OS_PAGE_SIZE), SLAB_SIZE - OS_PAGE_SIZE);
      }

      bool was_full = is_full();
      used--;

      assert(meta[0].is_unused());
      if (was_full || is_empty())
        return StatusChange;

      return NoStatusChange;
    }
  };
} // namespace snmalloc
