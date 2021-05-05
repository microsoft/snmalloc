#pragma once

#include "../ds/flaglock.h"
#include "../ds/helpers.h"
#include "../ds/mpmcstack.h"
#include "../pal/pal.h"
#include "address_space.h"
#include "allocstats.h"
#include "baseslab.h"
#include "sizeclass.h"

#include <new>
#include <string.h>

namespace snmalloc
{
  /**
   * This class allocates meta-data for the system.
   *
   * This is a place where additional protection such as
   * guard pages could be added to ensure that meta-data
   * is hard to corrupt.
   */
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL, typename ArenaMap>
  class MetadataAllocator
  {
  protected:
    using ASM = AddressSpaceManager<PAL, ArenaMap>;
    /**
     * Manages address space for this memory provider.
     */
    ASM address_space = {};

    /**
     * High-water mark of metadata used memory.
     */
    std::atomic<size_t> metadata_memory_used_bytes{0};

    /**
     * Special constructor used during initialisation to move a stack allocated
     * allocator into the heap.
     */
    MetadataAllocator(MetadataAllocator& meta)
    {
      address_space = std::move(meta.address_space);
      metadata_memory_used_bytes = meta.metadata_memory_used_bytes.exchange(0);
    }

    /**
     * Construct a memory provider owning some memory.  The PAL provided with
     * memory providers constructed in this way does not have to be able to
     * allocate memory, if the initial reservation is sufficient.
     */
    MetadataAllocator(CapPtr<void, CBChunk> start, size_t len)
    : address_space(start, len)
    {}

  public:
    /**
     * Default constructor.  This constructs a memory provider that doesn't yet
     * own any memory, but which can claim memory from the PAL.
     */
    MetadataAllocator() = default;

    /**
     * Primitive allocator for structures that are internal to the allocator.
     */
    template<typename T, size_t alignment, typename... Args>
    T* alloc_meta(Args&&... args)
    {
      // Cache line align
      size_t size = bits::align_up(sizeof(T), 64);
      size = bits::max(size, alignment);
      auto p = address_space.template reserve_with_left_over<true>(size);
      if (p == nullptr)
        return nullptr;

      metadata_memory_used_bytes += size;

      return new (p.unsafe_capptr) T(std::forward<Args>(args)...);
    }

    /**
     * Returns the underlying address space
     */
    ASM& get_address_space()
    {
      return address_space;
    }

    /**
     * Move assignment operator.  This should only be used during initialisation
     * of the system.  There should be no concurrency.
     */
    MetadataAllocator& operator=(MetadataAllocator&& other) noexcept
    {
      address_space = other.address_space;
      metadata_memory_used_bytes = other.metadata_memory_used_bytes.exchange(0);
      return *this;
    }
  };
} // namespace snmalloc
