#pragma once
#include "../mem/allocconfig.h"
#include "../pal/pal.h"
#include "address_space.h"
#include "pagemap.h"

namespace snmalloc
{
  /**
   * This class abstracts the platform to a consistent
   * way of handling memory for the front end.
   */
  class BackendAllocator
  {
  public:
    class LocalState
    {
      friend BackendAllocator;

      // TODO Separate meta data and object
      AddressSpaceManagerCore local_address_space;

      
    };

    template<typename PAL, bool fixed_range>
    class GlobalState
    {
      // TODO Separate meta data and object
      AddressSpaceManager<PAL> address_space;

      FlatPagemap<MIN_CHUNK_BITS, MetaEntry, PAL, fixed_range, &default_entry> pagemap;

    public:
      void init()
      {
        pagemap.init(&address_space);
      }
    };

  private:
    template<bool is_meta, typename SharedStateHandle>
    CapPtr<void, CBChunk> reserve(SharedStateHandle h, LocalState* local_state, size_t size)
    {
      // TODO have two address spaces.
      UNUSED(is_meta);

      CapPtr<void, CBChunk> p;
      if (local_state != nullptr)
      {
        p = local_state->local_address_space.reserve_with_left_over<typename SharedStateHandle::Pal>(size);
        if (p != nullptr)
          local_state->local_address_space.commit_block<typename SharedStateHandle::Pal>(p, size);
        else
        {
          auto& a = h.get_backend_state().address_space;
          auto refill_size = bits::max(
            size, bits::one_at_bit(21)); // TODO min and max heuristics
          auto refill = a.template reserve<false>(refill_size);
          if (refill == nullptr)
            return nullptr;
          local_state->local_address_space.add_range<typename SharedStateHandle::Pal>(refill, refill_size);
          // This should succeed
          p = local_state->local_address_space.reserve_with_left_over<typename SharedStateHandle::Pal>(size);
          if (p != nullptr)
            local_state->local_address_space.commit_block<typename SharedStateHandle::Pal>(p, size);
        }
      }
      else
      {
        auto& a = h.get_backend_state().address_space;
        p = a.template reserve_with_left_over<true>(size);
      }

      h.get_backend_state().address_space.add_peak_memory_usage(size);
      return p;
    }

  public:
    /**
     * Provide a block of meta-data with size and align.
     *
     * Backend allocator may use guard pages and separate area of
     * address space to protect this from corruption.
     */
    template<typename U, typename SharedStateHandle, typename... Args>
    static U* alloc_meta_data(
      SharedStateHandle h,
      LocalState* local_state,
      Args&&... args)
    {
      // Cache line align
      size_t size = bits::align_up(sizeof(U), 64);
      
      CapPtr<void, CBChunk> p = reserve<true>(h, local_state, size);

      if (p == nullptr)
        return nullptr;

      return new (p.unsafe_capptr) U(std::forward<Args>(args)...);
    }

    /**
     * Provide a power of 2 aligned and sized chunk of memory
     *
     * Set its metadata entry to t.
     */
    template<typename SharedStateHandle>
    static std::pair<CapPtr<void, CBChunk>, Metaslab*> alloc_slab(
      SharedStateHandle h,
      LocalState* local_state,
      size_t size,
      RemoteAllocator* remote,
      sizeclass_t sizeclass)
    {
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);

      CapPtr<void, CBChunk> p = reserve<false>(h, local_state, size);

#ifdef SNMALLOC_TRACING
      std::cout << "Alloc slab: " << p.unsafe_capptr << " (" << size << ")"
                << std::endl;
#endif
      if (p == nullptr)
      {
#ifdef SNMALLOC_TRACING
        std::cout << "Out of memory" << std::endl;
#endif
        return p;
      }

      CapPtr<void, CBChunk> meta = reserve<true>(h, local_state, sizeof(Metaslab));


      // TODO handle bounded versus lazy pagemaps in stats
      h.get_meta_address_space().add_peak_memory_usage(
        (size / MIN_CHUNK_SIZE) * sizeof(typename SharedStateHandle::Meta));

      for (address_t a = address_cast(p);
           a < address_cast(pointer_offset(p, size));
           a += MIN_CHUNK_SIZE)
      {
        h.get_backend_state().pagemap.add(a, t);
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
    static const typename SharedStateHandle::Meta& 
    get_meta_data(SharedStateHandle h, address_t p)
    {
      return h.get_backend_state().pagemap.template get<potentially_out_of_range>(p);
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
        h.get_backend_state().pagemap.set(a, t);
      }
    }
  };
}