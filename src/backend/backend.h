#pragma once
#include "../mem/allocconfig.h"
#include "../mem/metaslab.h"
#include "../pal/pal.h"
#include "address_space.h"
#include "commonconfig.h"
#include "pagemap.h"

#if defined(SNMALLOC_CHECK_CLIENT) && !defined(OPEN_ENCLAVE)
/**
 * Protect meta data blocks by allocating separate from chunks for
 * user allocations. This involves leaving gaps in address space.
 * This is less efficient, so should only be applied for the checked
 * build.
 *
 * On Open Enclave the address space is limited, so we disable this
 * feature.
 */
#  define SNMALLOC_META_PROTECTED
#endif

namespace snmalloc
{
  /**
   * This helper class implements the core functionality to allocate from an
   * address space and pagemap. Any backend implementation can use this class to
   * help with basic address space managment.
   */
  template<
    SNMALLOC_CONCEPT(ConceptPAL) PAL,
    typename LocalState,
    SNMALLOC_CONCEPT(ConceptBackendMetaRange) Pagemap>
  class AddressSpaceAllocatorCommon
  {
    // Size of local address space requests.  Currently aimed at 2MiB large
    // pages but should make this configurable (i.e. for OE, so we don't need as
    // much space).
#ifdef OPEN_ENCLAVE
    // Don't prefetch address space on OE, as it is limited.
    // This could cause perf issues during warm-up phases.
    constexpr static size_t LOCAL_CACHE_BLOCK = 0;
#else
    constexpr static size_t LOCAL_CACHE_BLOCK = bits::one_at_bit(21);
#endif

#ifdef SNMALLOC_META_PROTECTED
    // When protecting the meta-data, we use a smaller block for the meta-data
    // that is randomised inside a larger block.  This needs to be at least a
    // page so that we can use guard pages.
    constexpr static size_t LOCAL_CACHE_META_BLOCK =
      bits::max(MIN_CHUNK_SIZE * 2, OS_PAGE_SIZE);
    static_assert(
      LOCAL_CACHE_META_BLOCK <= LOCAL_CACHE_BLOCK,
      "LOCAL_CACHE_META_BLOCK must be smaller than LOCAL_CACHE_BLOCK");
#endif

  public:
    /**
     * Provide a block of meta-data with size and align.
     *
     * Backend allocator may use guard pages and separate area of
     * address space to protect this from corruption.
     */
    static capptr::Chunk<void> alloc_meta_data(
      AddressSpaceManager<PAL, Pagemap>& global,
      LocalState* local_state,
      size_t size)
    {
      return reserve<true>(global, local_state, size);
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
    static std::pair<capptr::Chunk<void>, Metaslab*> alloc_chunk(
      AddressSpaceManager<PAL, Pagemap>& global,
      LocalState* local_state,
      size_t size,
      RemoteAllocator* remote,
      sizeclass_t sizeclass)
    {
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);

      auto meta = reinterpret_cast<Metaslab*>(
        reserve<true>(global, local_state, sizeof(Metaslab)).unsafe_ptr());

      if (meta == nullptr)
        return {nullptr, nullptr};

      capptr::Chunk<void> p = reserve<false>(global, local_state, size);

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

      meta->meta_common.chunk = p;

      MetaEntry t(meta, remote, sizeclass);
      Pagemap::set_metaentry(address_cast(p), size, t);
      return {p, meta};
    }

  private:
    /**
     * Internal method for acquiring state from the local and global address
     * space managers.
     */
    template<bool is_meta>
    static capptr::Chunk<void> reserve(
      AddressSpaceManager<PAL, Pagemap>& global,
      LocalState* local_state,
      size_t size)
    {
#ifdef SNMALLOC_META_PROTECTED
      constexpr auto MAX_CACHED_SIZE =
        is_meta ? LOCAL_CACHE_META_BLOCK : LOCAL_CACHE_BLOCK;
#else
      constexpr auto MAX_CACHED_SIZE = LOCAL_CACHE_BLOCK;
#endif

      capptr::Chunk<void> p;
      if ((local_state != nullptr) && (size <= MAX_CACHED_SIZE))
      {
#ifdef SNMALLOC_META_PROTECTED
        auto& local = is_meta ? local_state->local_meta_address_space :
                                local_state->local_address_space;
#else
        auto& local = local_state->local_address_space;
#endif

        p = local.template reserve_with_left_over<PAL>(size);
        if (p != nullptr)
        {
          return p;
        }

        auto refill_size = LOCAL_CACHE_BLOCK;
        auto refill = global.template reserve<false>(refill_size);
        if (refill == nullptr)
          return nullptr;

#ifdef SNMALLOC_META_PROTECTED
        if (is_meta)
        {
          refill = sub_range(refill, LOCAL_CACHE_BLOCK, LOCAL_CACHE_META_BLOCK);
          refill_size = LOCAL_CACHE_META_BLOCK;
        }
#endif
        PAL::template notify_using<NoZero>(refill.unsafe_ptr(), refill_size);
        local.template add_range<PAL>(refill, refill_size);

        // This should succeed
        return local.template reserve_with_left_over<PAL>(size);
      }

#ifdef SNMALLOC_META_PROTECTED
      // During start up we need meta-data before we have a local allocator
      // This code protects that meta-data with randomisation, and guard pages.
      if (local_state == nullptr && is_meta)
      {
        size_t rsize = bits::max(OS_PAGE_SIZE, bits::next_pow2(size));
        size_t size_request = rsize * 64;

        p = global.template reserve<false>(size_request);
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

      p = global.template reserve_with_left_over<true>(size);
      return p;
    }

#ifdef SNMALLOC_META_PROTECTED
    /**
     * Returns a sub-range of [return, return+sub_size] that is contained in
     * the range [base, base+full_size]. The first and last slot are not used
     * so that the edges can be used for guard pages.
     */
    static capptr::Chunk<void>
    sub_range(capptr::Chunk<void> base, size_t full_size, size_t sub_size)
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
  };

  /**
   * This class implements the standard backend for handling allocations.
   * It abstracts page table management and address space management.
   */
  template<
    SNMALLOC_CONCEPT(ConceptPAL) PAL,
    bool fixed_range,
    typename PageMapEntry = MetaEntry>
  class BackendAllocator : public CommonConfig
  {
  public:
    using Pal = PAL;

    class Pagemap
    {
      friend class BackendAllocator;

      SNMALLOC_REQUIRE_CONSTINIT
      static inline FlatPagemap<MIN_CHUNK_BITS, PageMapEntry, PAL, fixed_range>
        concretePagemap;

    public:
      /**
       * Get the metadata associated with a chunk.
       *
       * Set template parameter to true if it not an error
       * to access a location that is not backed by a chunk.
       */
      template<bool potentially_out_of_range = false>
      SNMALLOC_FAST_PATH static const MetaEntry& get_metaentry(address_t p)
      {
        return concretePagemap.template get<potentially_out_of_range>(p);
      }

      /**
       * Get the metadata associated with a chunk.
       *
       * Set template parameter to true if it not an error
       * to access a location that is not backed by a chunk.
       */
      template<bool potentially_out_of_range = false>
      SNMALLOC_FAST_PATH static MetaEntry& get_metaentry_mut(address_t p)
      {
        return concretePagemap.template get_mut<potentially_out_of_range>(p);
      }

      /**
       * Set the metadata associated with a chunk.
       */
      SNMALLOC_FAST_PATH
      static void set_metaentry(address_t p, size_t size, const MetaEntry& t)
      {
        for (address_t a = p; a < p + size; a += MIN_CHUNK_SIZE)
        {
          concretePagemap.set(a, t);
        }
      }

      static void register_range(address_t p, size_t sz)
      {
        concretePagemap.register_range(p, sz);
      }

      /**
       * Return the bounds of the memory this back-end manages as a pair of
       * addresses (start then end).  This is available iff this is a
       * fixed-range Backend.
       */
      template<bool fixed_range_ = fixed_range>
      static SNMALLOC_FAST_PATH
        std::enable_if_t<fixed_range_, std::pair<address_t, address_t>>
        get_bounds()
      {
        static_assert(
          fixed_range_ == fixed_range, "Don't set SFINAE parameter!");

        return concretePagemap.get_bounds();
      }

      static bool is_initialised()
      {
        return concretePagemap.is_initialised();
      }
    };

    /**
     * Local state for the backend allocator.
     *
     * This class contains thread local structures to make the implementation
     * of the backend allocator more efficient.
     */
    class LocalState
    {
      template<
        SNMALLOC_CONCEPT(ConceptPAL) PAL2,
        typename LocalState,
        SNMALLOC_CONCEPT(ConceptBackendMetaRange) Pagemap2>
      friend class AddressSpaceAllocatorCommon;

      AddressSpaceManagerCore<Pagemap> local_address_space;

#ifdef SNMALLOC_META_PROTECTED
      /**
       * Secondary local address space, so we can apply some randomisation
       * and guard pages to protect the meta-data.
       */
      AddressSpaceManagerCore<Pagemap> local_meta_address_space;
#endif
    };

    SNMALLOC_REQUIRE_CONSTINIT
    static inline AddressSpaceManager<PAL, Pagemap> address_space;

  private:
    using AddressSpaceAllocator =
      AddressSpaceAllocatorCommon<Pal, LocalState, Pagemap>;

  public:
    template<bool fixed_range_ = fixed_range>
    static std::enable_if_t<!fixed_range_> init()
    {
      static_assert(fixed_range_ == fixed_range, "Don't set SFINAE parameter!");

      Pagemap::concretePagemap.init();
    }

    template<bool fixed_range_ = fixed_range>
    static std::enable_if_t<fixed_range_> init(void* base, size_t length)
    {
      static_assert(fixed_range_ == fixed_range, "Don't set SFINAE parameter!");

      auto [heap_base, heap_length] =
        Pagemap::concretePagemap.init(base, length);
      address_space.add_range(capptr::Chunk<void>(heap_base), heap_length);
    }

    /**
     * Provide a block of meta-data with size and align.
     *
     * Backend allocator may use guard pages and separate area of
     * address space to protect this from corruption.
     *
     * The template argument is the type of the metadata being allocated.  This
     * allows the backend to allocate different types of metadata in different
     * places or with different policies.
     */
    template<typename T>
    static capptr::Chunk<void>
    alloc_meta_data(LocalState* local_state, size_t size)
    {
      return AddressSpaceAllocator::alloc_meta_data(
        address_space, local_state, size);
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
    static std::pair<capptr::Chunk<void>, Metaslab*> alloc_chunk(
      LocalState* local_state,
      size_t size,
      RemoteAllocator* remote,
      sizeclass_t sizeclass)
    {
      return AddressSpaceAllocator::alloc_chunk(
        address_space, local_state, size, remote, sizeclass);
    }
  };
} // namespace snmalloc
