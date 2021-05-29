#pragma once
#include "address_space.h"
#include "pagemap.h"
#include "pal/pal.h"

namespace snmalloc
{
  static constexpr size_t MIN_CHUNK_BITS = 14;
  static constexpr size_t MIN_CHUNK_SIZE = bits::one_at_bit(MIN_CHUNK_BITS);

  /**
   * This class abstracts the platform to a consistent
   * way of handling memory for the front end.
   */
  class BackendAllocator
  {
  public:
    /**
     * Provide a block of meta-data with size and align.
     *
     * Backend allocator may use guard pages and separate area of
     * address space to protect this from corruption.
     */
    template<typename U, typename SharedStateHandle, typename... Args>
    static U* alloc_meta_data(SharedStateHandle h, Args&&... args)
    {
      // Cache line align
      size_t size = bits::align_up(sizeof(U), 64);
      auto& a = h.get_meta_address_space();
      auto p = a.template reserve_with_left_over<true>(size).unsafe_capptr;
      if (p == nullptr)
        return nullptr;

      //      metadata_memory_used_bytes += size;

      return new (p) U(std::forward<Args>(args)...);
    }

    /**
     * Provide a power of 2 aligned and sized chunk of memory
     *
     * Set its metadata entry to t.
     */
    template<typename SharedStateHandle>
    static CapPtr<void, CBChunk> alloc_slab(
      SharedStateHandle h, size_t size, typename SharedStateHandle::Meta t)
    {
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);
      auto p = h.get_object_address_space().template reserve<true>(size);
      for (address_t a = address_cast(p);
           a < address_cast(pointer_offset(p, size));
           a += MIN_CHUNK_SIZE)
      {
        h.get_pagemap().add(a, t);
      }
      return p;
    }

    /**
     * Get the metadata associated with a slab.
     *
     * Set template parameter to true if it not an error
     * to access a location that is not backed by a slab.
     */
    template<bool potentially_out_of_range = false, typename SharedStateHandle>
    static typename SharedStateHandle::Meta
    get_meta_data(SharedStateHandle h, address_t p)
    {
      return h.get_pagemap().template get<potentially_out_of_range>(p);
    }

    /**
     * Set the metadata associated with a slab.
     */
    template<typename SharedStateHandle>
    static void set_meta_data(
      SharedStateHandle h,
      address_t p,
      size_t size,
      typename SharedStateHandle::Meta t)
    {
      for (address_t a = p; a < p + size; a += MIN_CHUNK_SIZE)
      {
        h.get_pagemap().set(a, t);
      }
    }
  };
}