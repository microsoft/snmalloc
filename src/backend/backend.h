#pragma once
#include "../mem/allocconfig.h"
#include "../mem/metaslab.h"
#include "../pal/pal.h"
#include "address_space.h"
#include "pagemap.h"

namespace snmalloc
{
  /**
   * This class implements the standard backend for handling allocations.
   * It abstracts page table management and address space management.
   */
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL, bool fixed_range>
  class BackendAllocator
  {
  public:
    using Pal = PAL;

    /**
     * Local state for the backend allocator.
     *
     * This class contains thread local structures to make the implementation
     * of the backend allocator more efficient.
     */
    class LocalState
    {
      friend BackendAllocator;

      // TODO Separate meta data and object
      AddressSpaceManagerCore local_address_space;
    };

    /**
     * Global state for the backend allocator
     *
     * This contains the various global datastructures required to store
     * meta-data for each chunk of memory, and to provide well aligned chunks
     * of memory.
     *
     * This type is required by snmalloc to exist as part of the Backend.
     */
    class GlobalState
    {
      friend BackendAllocator;

      // TODO Separate meta data and object
      AddressSpaceManager<PAL> address_space;

      FlatPagemap<MIN_CHUNK_BITS, MetaEntry, PAL, fixed_range> pagemap;

    public:
      template<bool fixed_range_ = fixed_range>
      std::enable_if_t<!fixed_range_> init()
      {
        static_assert(
          fixed_range_ == fixed_range, "Don't set SFINAE parameter!");

        pagemap.init();

        if constexpr (fixed_range)
        {
          abort();
        }
      }

      template<bool fixed_range_ = fixed_range>
      std::enable_if_t<fixed_range_> init(void* base, size_t length)
      {
        static_assert(
          fixed_range_ == fixed_range, "Don't set SFINAE parameter!");

        auto [heap_base, heap_length] = pagemap.init(base, length);
        address_space.add_range(CapPtr<void, CBChunk>(heap_base), heap_length);

        if constexpr (!fixed_range)
        {
          abort();
        }
      }
    };

  private:
    /**
     * Internal method for acquiring state from the local and global address
     * space managers.
     */
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
          // TODO Improve heuristics and params
          auto refill_size = bits::max(size, bits::one_at_bit(21));
          auto refill = a.template reserve<false>(refill_size, h.pagemap);
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
        p = a.template reserve_with_left_over<true>(size, h.pagemap);
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
     * Returns a chunk of memory with alignment and size of `size`, and a
     * metaslab block.
     *
     * It additionally set the meta-data for this chunk of memory to
     * be
     *   (remote, sizeclass, metaslab)
     * where metaslab, is the second element of the pair return.
     */
    static std::pair<CapPtr<void, CBChunk>, Metaslab*> alloc_chunk(
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
      std::cout << "Alloc chunk: " << p.unsafe_ptr() << " (" << size << ")"
                << std::endl;
#endif
      if (p == nullptr)
      {
#ifdef SNMALLOC_TRACING
        std::cout << "Out of memory" << std::endl;
#endif
        return {p, nullptr};
      }

      auto meta = reinterpret_cast<Metaslab*>(
        reserve<true>(h, local_state, sizeof(Metaslab)).unsafe_ptr());

      MetaEntry t(meta, remote, sizeclass);

      for (address_t a = address_cast(p);
           a < address_cast(pointer_offset(p, size));
           a += MIN_CHUNK_SIZE)
      {
        h.pagemap.set(a, t);
      }
      return {p, meta};
    }

    /**
     * Get the metadata associated with a chunk.
     *
     * Set template parameter to true if it not an error
     * to access a location that is not backed by a chunk.
     */
    template<bool potentially_out_of_range = false>
    static const MetaEntry& get_meta_data(GlobalState& h, address_t p)
    {
      return h.pagemap.template get<potentially_out_of_range>(p);
    }

    /**
     * Set the metadata associated with a chunk.
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
} // namespace snmalloc
