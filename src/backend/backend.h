#pragma once
#include "../mem/allocconfig.h"
#include "../mem/metaslab.h"
#include "../pal/pal.h"
#include "address_space.h"
#include "pagemap.h"

namespace snmalloc
{
  /**
   * This class abstracts the platform to a consistent
   * way of handling memory for the front end.
   */
  template<typename PAL, bool fixed_range>
  class BackendAllocator
  {
  public:
    class LocalState
    {
      friend BackendAllocator;

      // TODO Separate meta data and object
      AddressSpaceManagerCore local_address_space;
    };

    class GlobalState
    {
      friend BackendAllocator;

      // TODO Separate meta data and object
      AddressSpaceManager<PAL> address_space;

      FlatPagemap<MIN_CHUNK_BITS, MetaEntry, PAL, fixed_range> pagemap;

    public:
      // TODO should check fixed range.
      void init()
      {
        pagemap.init(&address_space);
      }

      void init(CapPtr<void, CBChunk> base, size_t length)
      {
        address_space.add_range(base, length);
        pagemap.init(&address_space, address_cast(base), length);
      }
    };

  private:
    template<bool is_meta>
    static CapPtr<void, CBChunk>
    reserve(GlobalState& h, LocalState* local_state, size_t size)
    {
      // TODO have two address spaces.
      UNUSED(is_meta);

      CapPtr<void, CBChunk> p;
      if (local_state != nullptr)
      {
        p =
          local_state->local_address_space.template reserve_with_left_over<PAL>(
            size);
        if (p != nullptr)
          local_state->local_address_space.template commit_block<PAL>(p, size);
        else
        {
          auto& a = h.address_space;
          auto refill_size = bits::max(
            size, bits::one_at_bit(21)); // TODO min and max heuristics
          auto refill = a.template reserve<false>(refill_size);
          if (refill == nullptr)
            return nullptr;
          local_state->local_address_space.template add_range<PAL>(
            refill, refill_size);
          // This should succeed
          p = local_state->local_address_space
                .template reserve_with_left_over<PAL>(size);
          if (p != nullptr)
            local_state->local_address_space.template commit_block<PAL>(
              p, size);
        }
      }
      else
      {
        auto& a = h.address_space;
        p = a.template reserve_with_left_over<true>(size);
      }

      return p;
    }

  public:
    /**
     * Provide a block of meta-data with size and align.
     *
     * Backend allocator may use guard pages and separate area of
     * address space to protect this from corruption.
     */
    static CapPtr<void, CBChunk>
    alloc_meta_data(GlobalState& h, LocalState* local_state, size_t size)
    {
      return reserve<true>(h, local_state, size);
    }

    /**
     * Provide a power of 2 aligned and sized chunk of memory
     *
     * Set its metadata entry to t.
     */
    static std::pair<CapPtr<void, CBChunk>, Metaslab*> alloc_slab(
      GlobalState& h,
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
        return {p, nullptr};
      }

      Metaslab* meta = reinterpret_cast<Metaslab*>(
        reserve<true>(h, local_state, sizeof(Metaslab)).unsafe_capptr);

      MetaEntry t(meta, remote, sizeclass);

      for (address_t a = address_cast(p);
           a < address_cast(pointer_offset(p, size));
           a += MIN_CHUNK_SIZE)
      {
        h.pagemap.add(a, t);
      }
      return {p, meta};
    }

    /**
     * Get the metadata associated with a slab.
     *
     * Set template parameter to true if it not an error
     * to access a location that is not backed by a slab.
     */
    template<bool potentially_out_of_range = false>
    static const MetaEntry& get_meta_data(GlobalState& h, address_t p)
    {
      return h.pagemap.template get<potentially_out_of_range>(p);
    }

    /**
     * Set the metadata associated with a slab.
     */
    static void
    set_meta_data(GlobalState& h, address_t p, size_t size, MetaEntry t)
    {
      for (address_t a = p; a < p + size; a += MIN_CHUNK_SIZE)
      {
        h.pagemap.set(a, t);
      }
    }
  };
}