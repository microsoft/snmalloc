#pragma once

#include "../ds/dllist.h"
#include "allocconfig.h"
#include "allocslab.h"
#include "sizeclass.h"

namespace snmalloc
{
  class Mediumslab : public Allocslab
  {
    // This is the view of a 16 mb area when it is being used to allocate
    // medium sized classes: 64 kb to 16 mb, non-inclusive.
  private:
    friend DLList<Mediumslab>;

    // Keep the allocator pointer on a separate cache line. It is read by
    // other threads, and does not change, so we avoid false sharing.
    alignas(CACHELINE_SIZE) Mediumslab* next;
    Mediumslab* prev;

    uint16_t free;
    uint8_t head;
    uint8_t sizeclass;
    uint16_t stack[SLAB_COUNT - 1];

  public:
    static constexpr uint32_t header_size()
    {
      static_assert(
        sizeof(Mediumslab) < OS_PAGE_SIZE,
        "Mediumslab header size must be less than the page size");
      static_assert(
        sizeof(Mediumslab) < SLAB_SIZE,
        "Mediumslab header size must be less than the slab size");

      // Always use a full page as the header, in order to get page sized
      // alignment of individual allocations.
      return OS_PAGE_SIZE;
    }

    static Mediumslab* get(const void* p)
    {
      return pointer_align_down<SUPERSLAB_SIZE, Mediumslab>(
        const_cast<void*>(p));
    }

    void init(RemoteAllocator* alloc, sizeclass_t sc, size_t rsize)
    {
      SNMALLOC_ASSERT(sc >= NUM_SMALL_CLASSES);
      SNMALLOC_ASSERT((sc - NUM_SMALL_CLASSES) < NUM_MEDIUM_CLASSES);

      allocator = alloc;
      head = 0;

      // If this was previously a Mediumslab of the same sizeclass, don't
      // initialise the allocation stack.
      if ((kind != Medium) || (sizeclass != sc))
      {
        sizeclass = static_cast<uint8_t>(sc);
        uint16_t ssize = static_cast<uint16_t>(rsize >> 8);
        kind = Medium;
        free = medium_slab_free(sc);
        for (uint16_t i = free; i > 0; i--)
          stack[free - i] =
            static_cast<uint16_t>((SUPERSLAB_SIZE >> 8) - (i * ssize));
      }
      else
      {
        SNMALLOC_ASSERT(free == medium_slab_free(sc));
      }
    }

    uint8_t get_sizeclass()
    {
      return sizeclass;
    }

    template<ZeroMem zero_mem, SNMALLOC_CONCEPT(ConceptPAL) PAL>
    void* alloc(size_t size)
    {
      SNMALLOC_ASSERT(!full());

      uint16_t index = stack[head++];
      void* p = pointer_offset(this, (static_cast<size_t>(index) << 8));
      free--;

      if constexpr (zero_mem == YesZero)
        PAL::zero(p, size);
      else
        UNUSED(size);

      return p;
    }

    bool dealloc(void* p)
    {
      SNMALLOC_ASSERT(head > 0);

      // Returns true if the Mediumslab was full before this deallocation.
      bool was_full = full();
      free++;
      stack[--head] = pointer_to_index(p);

      return was_full;
    }

    bool full()
    {
      return free == 0;
    }

    bool empty()
    {
      return head == 0;
    }

  private:
    uint16_t pointer_to_index(void* p)
    {
      // Get the offset from the slab for a memory location.
      return static_cast<uint16_t>(pointer_diff(this, p) >> 8);
    }
  };
} // namespace snmalloc
