#pragma once
#include "../ds/address.h"
#include "../ds/flaglock.h"
#include "../pal/pal.h"

#include <array>
#ifdef SNMALLOC_TRACING
#  include <iostream>
#endif

namespace snmalloc
{
  /**
   * Implements a power of two allocator, where all blocks are aligned to the
   * same power of two as their size. This is what snmalloc uses to get
   * alignment of very large sizeclasses.
   *
   * It cannot unreserve memory, so this does not require the
   * usual complexity of a buddy allocator.
   */
  class AddressSpaceManagerCore
  {
    struct FreeChunk
    {
      CapPtr<FreeChunk, CBChunk> next;
    };

    /**
     * Stores the blocks of address space
     *
     * The array indexes based on power of two size.
     *
     * The entries for each size form a linked list.  For sizes below
     * MIN_CHUNK_SIZE they are linked through the first location in the
     * block of memory.  For sizes of, and above, MIN_CHUNK_SIZE they are
     * linked using the pagemap. We only use the smaller than MIN_CHUNK_SIZE
     * allocations for meta-data, so we can be sure that the next pointers
     * never occur in a blocks that are ultimately used for object allocations.
     *
     * bits::BITS is used for simplicity, we do not use below the pointer size,
     * and large entries will be unlikely to be supported by the platform.
     */
    std::array<CapPtr<FreeChunk, CBChunk>, bits::BITS> ranges = {};

    /**
     * Checks a block satisfies its invariant.
     */
    inline void check_block(CapPtr<FreeChunk, CBChunk> base, size_t align_bits)
    {
      SNMALLOC_ASSERT(
        address_cast(base) ==
        bits::align_up(address_cast(base), bits::one_at_bit(align_bits)));
      // All blocks need to be bigger than a pointer.
      SNMALLOC_ASSERT(bits::one_at_bit(align_bits) >= sizeof(void*));
      UNUSED(base);
      UNUSED(align_bits);
    }

    /**
     * Set next pointer for a power of two address range.
     *
     * This abstracts the use of either
     * - the pagemap; or
     * - the first pointer word of the block
     * to store the next pointer for the list of unused address space of a
     * particular size.
     */
    template<typename Pagemap>
    void set_next(
      size_t align_bits,
      CapPtr<FreeChunk, CBChunk> base,
      CapPtr<FreeChunk, CBChunk> next,
      Pagemap& pagemap)
    {
      if (align_bits >= MIN_CHUNK_BITS)
      {
        // The pagemap stores MetaEntrys, abuse the metaslab field to be the
        // next block in the stack of blocks.
        //
        // The pagemap entries here have nullptr (i.e., fake_large_remote) as
        // their remote, and so other accesses to the pagemap (by external_pointer,
        // for example) will not attempt to follow this "Metaslab" pointer.
        MetaEntry t(reinterpret_cast<Metaslab*>(next.unsafe_ptr()), nullptr, 0);
        pagemap.set(address_cast(base), t);
        return;
      }

      base->next = next;
    }

    /**
     * Get next pointer for a power of two address range.
     *
     * This abstracts the use of either
     * - the pagemap; or
     * - the first pointer word of the block
     * to store the next pointer for the list of unused address space of a
     * particular size.
     */
    template<typename Pagemap>
    CapPtr<FreeChunk, CBChunk> get_next(
      size_t align_bits, CapPtr<FreeChunk, CBChunk> base, Pagemap& pagemap)
    {
      if (align_bits >= MIN_CHUNK_BITS)
      {
        const MetaEntry& t = pagemap.template get<false>(address_cast(base));
        return CapPtr<FreeChunk, CBChunk>(
          reinterpret_cast<FreeChunk*>(t.get_metaslab()));
      }

      return base->next;
    }

    /**
     * Adds a block to `ranges`.
     */
    template<SNMALLOC_CONCEPT(ConceptPAL) PAL, typename Pagemap>
    void add_block(
      size_t align_bits, CapPtr<FreeChunk, CBChunk> base, Pagemap& pagemap)
    {
      check_block(base, align_bits);
      SNMALLOC_ASSERT(align_bits < 64);

      set_next(align_bits, base, ranges[align_bits], pagemap);
      ranges[align_bits] = base.as_static<FreeChunk>();
    }

    /**
     * Find a block of the correct size. May split larger blocks
     * to satisfy this request.
     */
    template<SNMALLOC_CONCEPT(ConceptPAL) PAL, typename Pagemap>
    CapPtr<void, CBChunk> remove_block(size_t align_bits, Pagemap& pagemap)
    {
      CapPtr<FreeChunk, CBChunk> first = ranges[align_bits];
      if (first == nullptr)
      {
        if (align_bits == (bits::BITS - 1))
        {
          // Out of memory
          return nullptr;
        }

        // Look for larger block and split up recursively
        CapPtr<void, CBChunk> bigger =
          remove_block<PAL>(align_bits + 1, pagemap);
        if (bigger != nullptr)
        {
          // This block is going to be broken up into sub CHUNK_SIZE blocks
          // so we need to commit it to enable the next pointers to be used
          // inside the block.
          if ((align_bits + 1) == MIN_CHUNK_BITS)
          {
            commit_block<PAL>(bigger, MIN_CHUNK_SIZE);
          }

          size_t left_over_size = bits::one_at_bit(align_bits);
          auto left_over = pointer_offset(bigger, left_over_size);

          add_block<PAL>(
            align_bits,
            Aal::capptr_bound<FreeChunk, CBChunk>(left_over, left_over_size),
            pagemap);
          check_block(left_over.as_static<FreeChunk>(), align_bits);
        }
        check_block(bigger.as_static<FreeChunk>(), align_bits + 1);
        return bigger;
      }

      check_block(first, align_bits);
      ranges[align_bits] = get_next(align_bits, first, pagemap);
      return first.as_void();
    }

  public:
    /**
     * Add a range of memory to the address space.
     * Divides blocks into power of two sizes with natural alignment
     */
    template<SNMALLOC_CONCEPT(ConceptPAL) PAL, typename Pagemap>
    void add_range(CapPtr<void, CBChunk> base, size_t length, Pagemap& pagemap)
    {
      // For start and end that are not chunk sized, we need to
      // commit the pages to track the allocations.
      auto base_chunk = pointer_align_up(base, MIN_CHUNK_SIZE);
      auto end = pointer_offset(base, length);
      auto end_chunk = pointer_align_down(end, MIN_CHUNK_SIZE);
      auto start_length = pointer_diff(base, base_chunk);
      auto end_length = pointer_diff(end_chunk, end);
      if (start_length != 0)
        commit_block<PAL>(base, start_length);
      if (end_length != 0)
        commit_block<PAL>(end_chunk, end_length);

      // Find the minimum set of maximally aligned blocks in this range.
      // Each block's alignment and size are equal.
      while (length >= sizeof(void*))
      {
        size_t base_align_bits = bits::ctz(address_cast(base));
        size_t length_align_bits = (bits::BITS - 1) - bits::clz(length);
        size_t align_bits = bits::min(base_align_bits, length_align_bits);
        size_t align = bits::one_at_bit(align_bits);
        auto b = base.as_static<FreeChunk>();

        check_block(b, align_bits);
        add_block<PAL>(align_bits, b, pagemap);

        base = pointer_offset(base, align);
        length -= align;
      }
    }

    /**
     * Commit a block of memory
     */
    template<SNMALLOC_CONCEPT(ConceptPAL) PAL>
    void commit_block(CapPtr<void, CBChunk> base, size_t size)
    {
      // Rounding required for sub-page allocations.
      auto page_start = pointer_align_down<OS_PAGE_SIZE, char>(base);
      auto page_end =
        pointer_align_up<OS_PAGE_SIZE, char>(pointer_offset(base, size));
      size_t using_size = pointer_diff(page_start, page_end);
      PAL::template notify_using<NoZero>(page_start.unsafe_ptr(), using_size);
    }

    /**
     * Returns a pointer to a block of memory of the supplied size.
     * The block will be committed, if specified by the template parameter.
     * The returned block is guaranteed to be aligened to the size.
     *
     * Only request 2^n sizes, and not less than a pointer.
     *
     * On StrictProvenance architectures, any underlying allocations made as
     * part of satisfying the request will be registered with the provided
     * arena_map for use in subsequent amplification.
     */
    template<SNMALLOC_CONCEPT(ConceptPAL) PAL, typename Pagemap>
    CapPtr<void, CBChunk> reserve(size_t size, Pagemap& pagemap)
    {
#ifdef SNMALLOC_TRACING
      std::cout << "ASM Core reserve request:" << size << std::endl;
#endif

      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(size >= sizeof(void*));

      return remove_block<PAL>(bits::next_pow2_bits(size), pagemap);
    }

    /**
     * Aligns block to next power of 2 above size, and unused space at the end
     * of the block is retained by the address space manager.
     *
     * This is useful for allowing the space required for alignment to be
     * used, by smaller objects.
     */
    template<SNMALLOC_CONCEPT(ConceptPAL) PAL, typename Pagemap>
    CapPtr<void, CBChunk> reserve_with_left_over(size_t size, Pagemap& pagemap)
    {
      SNMALLOC_ASSERT(size >= sizeof(void*));

      size = bits::align_up(size, sizeof(void*));

      size_t rsize = bits::next_pow2(size);

      auto res = reserve<PAL>(rsize, pagemap);

      if (res != nullptr)
      {
        if (rsize > size)
        {
          add_range<PAL>(pointer_offset(res, size), rsize - size, pagemap);
        }
      }
      return res;
    }

    /**
     * Default constructor.  An address-space manager constructed in this way
     * does not own any memory at the start and will request any that it needs
     * from the PAL.
     */
    AddressSpaceManagerCore() = default;
  };
} // namespace snmalloc
