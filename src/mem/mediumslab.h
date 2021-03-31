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
    static constexpr size_t header_size()
    {
      static_assert(
        sizeof(Mediumslab) < OS_PAGE_SIZE,
        "Mediumslab header size must be less than the page size");
      static_assert(
        sizeof(Mediumslab) < SLAB_SIZE,
        "Mediumslab header size must be less than the slab size");

      /*
       * Always use a full page or SLAB, whichever is smaller, in order
       * to get good alignment of individual allocations.  Some platforms
       * have huge minimum pages (e.g., Linux on PowerPC uses 64KiB) and
       * our SLABs are occasionally small by comparison (e.g., in OE, when
       * we take them to be 8KiB).
       */
      return bits::align_up(sizeof(Mediumslab), min(OS_PAGE_SIZE, SLAB_SIZE));
    }

    static Mediumslab* get(const void* p)
    {
      return pointer_align_down<SUPERSLAB_SIZE, Mediumslab>(
        const_cast<void*>(p));
    }

    // This is pre-factored to take an explicit self parameter so that we can
    // eventually annotate that pointer with additional information.
    static void
    init(Mediumslab* self, RemoteAllocator* alloc, sizeclass_t sc, size_t rsize)
    {
      SNMALLOC_ASSERT(sc >= NUM_SMALL_CLASSES);
      SNMALLOC_ASSERT((sc - NUM_SMALL_CLASSES) < NUM_MEDIUM_CLASSES);

      self->allocator = alloc;
      self->head = 0;

      // If this was previously a Mediumslab of the same sizeclass, don't
      // initialise the allocation stack.
      if ((self->kind != Medium) || (self->sizeclass != sc))
      {
        self->sizeclass = static_cast<uint8_t>(sc);
        uint16_t ssize = static_cast<uint16_t>(rsize >> 8);
        self->kind = Medium;
        self->free = medium_slab_free(sc);
        for (uint16_t i = self->free; i > 0; i--)
          self->stack[self->free - i] =
            static_cast<uint16_t>((SUPERSLAB_SIZE >> 8) - (i * ssize));
      }
      else
      {
        SNMALLOC_ASSERT(self->free == medium_slab_free(sc));
      }
    }

    uint8_t get_sizeclass()
    {
      return sizeclass;
    }

    template<ZeroMem zero_mem, SNMALLOC_CONCEPT(ConceptPAL) PAL>
    static void* alloc(Mediumslab* self, size_t size)
    {
      SNMALLOC_ASSERT(!full(self));

      uint16_t index = self->stack[self->head++];
      void* p = pointer_offset(self, (static_cast<size_t>(index) << 8));
      self->free--;

      if constexpr (zero_mem == YesZero)
        pal_zero<PAL>(p, size);
      else
        UNUSED(size);

      return p;
    }

    static bool dealloc(Mediumslab* self, void* p)
    {
      SNMALLOC_ASSERT(self->head > 0);

      // Returns true if the Mediumslab was full before this deallocation.
      bool was_full = full(self);
      self->free++;
      self->stack[--(self->head)] = self->address_to_index(address_cast(p));

      return was_full;
    }

    static bool full(Mediumslab* self)
    {
      return self->free == 0;
    }

    static bool empty(Mediumslab* self)
    {
      return self->head == 0;
    }

  private:
    uint16_t address_to_index(address_t p)
    {
      // Get the offset from the slab for a memory location.
      return static_cast<uint16_t>((p - address_cast(this)) >> 8);
    }
  };
} // namespace snmalloc
