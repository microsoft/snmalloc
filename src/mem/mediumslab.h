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
    friend DLList<Mediumslab, CapPtrCBChunkE>;

    // Keep the allocator pointer on a separate cache line. It is read by
    // other threads, and does not change, so we avoid false sharing.
    alignas(CACHELINE_SIZE) CapPtr<Mediumslab, CBChunkE> next;
    CapPtr<Mediumslab, CBChunkE> prev;

    // Store a pointer to ourselves without platform constraints applied,
    // as we need this to be able to zero memory by manipulating the VM map
    CapPtr<void, CBChunk> self_chunk;

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

    /**
     * Given a highly-privileged pointer pointing to or within an object in
     * this slab, return a pointer to the slab headers.
     *
     * In debug builds on StrictProvenance architectures, we will enforce the
     * slab bounds on this returned pointer.  In non-debug builds, we will
     * return a highly-privileged pointer (i.e., CBArena) instead as these
     * pointers are not exposed from the allocator.
     */
    template<typename T>
    static SNMALLOC_FAST_PATH CapPtr<Mediumslab, CBChunkD>
    get(CapPtr<T, CBArena> p)
    {
      return capptr_bound_chunkd(
        pointer_align_down<SUPERSLAB_SIZE, Mediumslab>(p.as_void()),
        SUPERSLAB_SIZE);
    }

    static void init(
      CapPtr<Mediumslab, CBChunk> self,
      RemoteAllocator* alloc,
      sizeclass_t sc,
      size_t rsize)
    {
      SNMALLOC_ASSERT(sc >= NUM_SMALL_CLASSES);
      SNMALLOC_ASSERT((sc - NUM_SMALL_CLASSES) < NUM_MEDIUM_CLASSES);

      self->allocator = alloc;
      self->head = 0;

      // If this was previously a Mediumslab of the same sizeclass, don't
      // initialise the allocation stack.
      if ((self->kind != Medium) || (self->sizeclass != sc))
      {
        self->self_chunk = self.as_void();
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
        SNMALLOC_ASSERT(self->self_chunk == self.as_void());
      }
    }

    uint8_t get_sizeclass()
    {
      return sizeclass;
    }

    template<ZeroMem zero_mem, SNMALLOC_CONCEPT(ConceptPAL) PAL>
    static CapPtr<void, CBAllocE>
    alloc(CapPtr<Mediumslab, CBChunkE> self, size_t size)
    {
      SNMALLOC_ASSERT(!full(self));

      uint16_t index = self->stack[self->head++];
      auto p = pointer_offset(self, (static_cast<size_t>(index) << 8));
      self->free--;

      if constexpr (zero_mem == YesZero)
        pal_zero<PAL>(Aal::capptr_rebound(self->self_chunk, p), size);
      else
        UNUSED(size);

      return Aal::capptr_bound<void, CBAllocE>(p, size);
    }

    static bool
    dealloc(CapPtr<Mediumslab, CBChunkD> self, CapPtr<void, CBAlloc> p)
    {
      SNMALLOC_ASSERT(self->head > 0);

      // Returns true if the Mediumslab was full before this deallocation.
      bool was_full = full(self);
      self->free++;
      self->stack[--(self->head)] = self->address_to_index(address_cast(p));

      return was_full;
    }

    template<SNMALLOC_CONCEPT(capptr_bounds::c) B>
    static bool full(CapPtr<Mediumslab, B> self)
    {
      return self->free == 0;
    }

    template<SNMALLOC_CONCEPT(capptr_bounds::c) B>
    static bool empty(CapPtr<Mediumslab, B> self)
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
