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
  template<
    SNMALLOC_CONCEPT(ConceptPAL) PAL,
    bool fixed_range,
    typename PageMapEntry = MetaEntry>
  class BackendAllocator
  {
    // Size of local address space requests.  Currently aimed at 2MiB large
    // pages but should make this configurable (i.e. for OE, so we don't need as
    // much space).
    constexpr static size_t LOCAL_CACHE_BLOCK = bits::one_at_bit(21);

#ifdef SNMALLOC_CHECK_CLIENT
    // When protecting the meta-data, we use a smaller block for the meta-data
    // that is randomised inside a larger block.  This needs to be at least a
    // page so that we can use guard pages.
    constexpr static size_t LOCAL_CACHE_META_BLOCK =
      bits::max(MIN_CHUNK_SIZE * 2, OS_PAGE_SIZE);
#endif

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

      AddressSpaceManagerCore local_address_space;

#ifdef SNMALLOC_CHECK_CLIENT
      /**
       * Secondary local address space, so we can apply some randomisation
       * and guard pages to protect the meta-data.
       */
      AddressSpaceManagerCore local_meta_address_space;
#endif
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

    protected:
      AddressSpaceManager<PAL> address_space;

      FlatPagemap<MIN_CHUNK_BITS, PageMapEntry, PAL, fixed_range> pagemap;

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
        address_space.add_range(
          CapPtr<void, CBChunk>(heap_base), heap_length, pagemap);

        if constexpr (!fixed_range)
        {
          abort();
        }
      }
    };

  private:
#ifdef SNMALLOC_CHECK_CLIENT
    /**
     * Returns a sub-range of [return, return+sub_size] that is contained in
     * the range [base, base+full_size]. The first and last slot are not used
     * so that the edges can be used for guard pages.
     */
    static CapPtr<void, CBChunk>
    sub_range(CapPtr<void, CBChunk> base, size_t full_size, size_t sub_size)
    {
      SNMALLOC_ASSERT(bits::is_pow2(full_size));
      SNMALLOC_ASSERT(bits::is_pow2(sub_size));
      SNMALLOC_ASSERT(full_size % sub_size == 0);
      SNMALLOC_ASSERT(full_size / sub_size >= 4);

      size_t offset_mask = full_size - sub_size;

      // Don't use first or last block in the larger reservation
      // Loop required to get uniform distribution.
      size_t offset;
      do
      {
        offset = get_entropy64<PAL>() & offset_mask;
      } while ((offset == 0) || (offset == offset_mask));

      return pointer_offset(base, offset);
    }
#endif

    /**
     * Internal method for acquiring state from the local and global address
     * space managers.
     */
    template<bool is_meta>
    static CapPtr<void, CBChunk>
    reserve(GlobalState& h, LocalState* local_state, size_t size)
    {
#ifdef SNMALLOC_CHECK_CLIENT
      constexpr auto MAX_CACHED_SIZE =
        is_meta ? LOCAL_CACHE_META_BLOCK : LOCAL_CACHE_BLOCK;
#else
      constexpr auto MAX_CACHED_SIZE = LOCAL_CACHE_BLOCK;
#endif

      auto& global = h.address_space;

      CapPtr<void, CBChunk> p;
      if ((local_state != nullptr) && (size <= MAX_CACHED_SIZE))
      {
#ifdef SNMALLOC_CHECK_CLIENT
        auto& local = is_meta ? local_state->local_meta_address_space :
                                local_state->local_address_space;
#else
        auto& local = local_state->local_address_space;
#endif

        p = local.template reserve_with_left_over<PAL>(size, h.pagemap);
        if (p != nullptr)
        {
          return p;
        }

        auto refill_size = LOCAL_CACHE_BLOCK;
        auto refill = global.template reserve<false>(refill_size, h.pagemap);
        if (refill == nullptr)
          return nullptr;

#ifdef SNMALLOC_CHECK_CLIENT
        if (is_meta)
        {
          refill = sub_range(refill, LOCAL_CACHE_BLOCK, LOCAL_CACHE_META_BLOCK);
          refill_size = LOCAL_CACHE_META_BLOCK;
        }
#endif
        PAL::template notify_using<NoZero>(refill.unsafe_ptr(), refill_size);
        local.template add_range<PAL>(refill, refill_size, h.pagemap);

        // This should succeed
        return local.template reserve_with_left_over<PAL>(size, h.pagemap);
      }

#ifdef SNMALLOC_CHECK_CLIENT
      // During start up we need meta-data before we have a local allocator
      // This code protects that meta-data with randomisation, and guard pages.
      if (local_state == nullptr && is_meta)
      {
        size_t rsize = bits::max(OS_PAGE_SIZE, bits::next_pow2(size));
        size_t size_request = rsize * 64;

        p = global.template reserve<false>(size_request, h.pagemap);
        if (p == nullptr)
          return nullptr;

        p = sub_range(p, size_request, rsize);

        PAL::template notify_using<NoZero>(p.unsafe_ptr(), rsize);
        return p;
      }

      // This path does not apply any guard pages to very large
      // meta data requests.  There are currently no meta data-requests
      // this large.  This assert checks for this assumption breaking.
      SNMALLOC_ASSERT(!is_meta);
#endif

      p = global.template reserve_with_left_over<true>(size, h.pagemap);
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

      auto meta = reinterpret_cast<Metaslab*>(
        reserve<true>(h, local_state, sizeof(Metaslab)).unsafe_ptr());

      if (meta == nullptr)
        return {nullptr, nullptr};

      CapPtr<void, CBChunk> p = reserve<false>(h, local_state, size);

#ifdef SNMALLOC_TRACING
      std::cout << "Alloc chunk: " << p.unsafe_ptr() << " (" << size << ")"
                << std::endl;
#endif
      if (p == nullptr)
      {
        // TODO: This is leaking `meta`. Currently there is no facility for
        // meta-data reuse, so will leave until we develop more expressive
        // meta-data management.
#ifdef SNMALLOC_TRACING
        std::cout << "Out of memory" << std::endl;
#endif
        return {p, nullptr};
      }

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
