#pragma once

#include "../ds/address.h"
#include "../ds/flaglock.h"
#include "../pal/pal.h"
//#include "arenamap.h"

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
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL /*, typename ArenaMap*/>
  class AddressSpaceManager
  {
    /**
     * Instantiate the ArenaMap here.
     *
     * In most cases, this will be a purely static object (a DefaultArenaMap
     * using a GlobalPagemapTemplate or ExternalGlobalPagemapTemplate).  For
     * sandboxes, this may have per-instance state (e.g., the sandbox root);
     * presently, that's handled by the AddressSpaceManager constructor
     * that takes a pointer to address space it owns.  There is some
     * non-orthogonality of concerns here.
     */
    //    ArenaMap arena_map = {};

    /**
     * Stores the blocks of address space
     *
     * The first level of array indexes based on power of two size.
     *
     * The first entry ranges[n][0] is just a pointer to an address range
     * of size 2^n.
     *
     * The second entry ranges[n][1] is a pointer to a linked list of blocks
     * of this size. The final block in the list is not committed, so we commit
     * on pop for this corner case.
     *
     * Invariants
     *  ranges[n][1] != nullptr => ranges[n][0] != nullptr
     *
     * bits::BITS is used for simplicity, we do not use below the pointer size,
     * and large entries will be unlikely to be supported by the platform.
     */
    std::array<std::array<CapPtr<void, CBChunk>, 2>, bits::BITS> ranges = {};

    /**
     * This is infrequently used code, a spin lock simplifies the code
     * considerably, and should never be on the fast path.
     */
    std::atomic_flag spin_lock = ATOMIC_FLAG_INIT;

    /**
     * Checks a block satisfies its invariant.
     */
    inline void check_block(CapPtr<void, CBChunk> base, size_t align_bits)
    {
      SNMALLOC_ASSERT(
        base == pointer_align_up(base, bits::one_at_bit(align_bits)));
      // All blocks need to be bigger than a pointer.
      SNMALLOC_ASSERT(bits::one_at_bit(align_bits) >= sizeof(void*));
      UNUSED(base);
      UNUSED(align_bits);
    }

    /**
     * Adds a block to `ranges`.
     */
    void add_block(size_t align_bits, CapPtr<void, CBChunk> base)
    {
      check_block(base, align_bits);
      SNMALLOC_ASSERT(align_bits < 64);
      if (ranges[align_bits][0] == nullptr)
      {
        // Prefer first slot if available.
        ranges[align_bits][0] = base;
        return;
      }

      if (ranges[align_bits][1] != nullptr)
      {
#ifdef SNMALLOC_TRACING
        std::cout << "Add range linking." << std::endl;
#endif
        // Add to linked list.
        commit_block(base, sizeof(void*));
        *(base.template as_static<CapPtr<void, CBChunk>>().unsafe_capptr) =
          ranges[align_bits][1];
        check_block(ranges[align_bits][1], align_bits);
      }

      // Update head of list
      ranges[align_bits][1] = base;
      check_block(ranges[align_bits][1], align_bits);
    }

    /**
     * Find a block of the correct size. May split larger blocks
     * to satisfy this request.
     */
    CapPtr<void, CBChunk> remove_block(size_t align_bits)
    {
      CapPtr<void, CBChunk> first = ranges[align_bits][0];
      if (first == nullptr)
      {
        if (align_bits == (bits::BITS - 1))
        {
          // Out of memory
          return nullptr;
        }

        // Look for larger block and split up recursively
        CapPtr<void, CBChunk> bigger = remove_block(align_bits + 1);
        if (bigger != nullptr)
        {
          size_t left_over_size = bits::one_at_bit(align_bits);
          auto left_over = pointer_offset(bigger, left_over_size);
          ranges[align_bits][0] =
            Aal::capptr_bound<void, CBChunk>(left_over, left_over_size);
          check_block(left_over, align_bits);
        }
        check_block(bigger, align_bits + 1);
        return bigger;
      }

      CapPtr<void, CBChunk> second = ranges[align_bits][1];
      if (second != nullptr)
      {
        commit_block(second, sizeof(void*));
        auto psecond =
          second.template as_static<CapPtr<void, CBChunk>>().unsafe_capptr;
        auto next = *psecond;
        ranges[align_bits][1] = next;
        // Zero memory. Client assumes memory contains only zeros.
        *psecond = nullptr;
        check_block(second, align_bits);
        check_block(next, align_bits);
        return second;
      }

      check_block(first, align_bits);
      ranges[align_bits][0] = nullptr;
      return first;
    }

    /**
     * Add a range of memory to the address space.
     * Divides blocks into power of two sizes with natural alignment
     */
    void add_range(CapPtr<void, CBChunk> base, size_t length)
    {
      // Find the minimum set of maximally aligned blocks in this range.
      // Each block's alignment and size are equal.
      while (length >= sizeof(void*))
      {
        size_t base_align_bits = bits::ctz(address_cast(base));
        size_t length_align_bits = (bits::BITS - 1) - bits::clz(length);
        size_t align_bits = bits::min(base_align_bits, length_align_bits);
        size_t align = bits::one_at_bit(align_bits);

        check_block(base, align_bits);
        add_block(align_bits, base);

        base = pointer_offset(base, align);
        length -= align;
      }
    }

    /**
     * Commit a block of memory
     */
    void commit_block(CapPtr<void, CBChunk> base, size_t size)
    {
      // Rounding required for sub-page allocations.
      auto page_start = pointer_align_down<OS_PAGE_SIZE, char>(base);
      auto page_end =
        pointer_align_up<OS_PAGE_SIZE, char>(pointer_offset(base, size));
      size_t using_size = pointer_diff(page_start, page_end);
      PAL::template notify_using<NoZero>(page_start.unsafe_capptr, using_size);
    }

  public:
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
    template<bool committed, bool align = true>
    CapPtr<void, CBChunk> reserve(size_t size)
    {
#ifdef SNMALLOC_TRACING
      std::cout << "ASM reserve request:" << size << std::endl;
#endif

      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(size >= sizeof(void*));

      if constexpr (align == false)
      {
        if constexpr (
        pal_supports<AlignedAllocation, PAL>)
          return CapPtr<void, CBChunk>(
              PAL::template reserve_aligned<committed>(size));
        else
        {
          auto [block, size2] = PAL::reserve_at_least(size);
          // TODO wasting size here.
#ifdef SNMALLOC_TRACING
          std::cout << "Unaligned alloc here:" << block << " (" << size2 << ")" << std::endl;
#endif
          return CapPtr<void, CBChunk>(block);
        }
      }

      /*
       * For sufficiently large allocations with platforms that support aligned
       * allocations and architectures that don't require StrictProvenance,
       * try asking the platform first.
       */
      if constexpr (
        pal_supports<AlignedAllocation, PAL> && !aal_supports<StrictProvenance>)
      {
        if (size >= PAL::minimum_alloc_size)
          return CapPtr<void, CBChunk>(
            PAL::template reserve_aligned<committed>(size));
      }

      CapPtr<void, CBChunk> res;
      {
        FlagLock lock(spin_lock);
        res = remove_block(bits::next_pow2_bits(size));
        if (res == nullptr)
        {
          // Allocation failed ask OS for more memory
          CapPtr<void, CBChunk> block = nullptr;
          size_t block_size = 0;
          if constexpr (pal_supports<AlignedAllocation, PAL>)
          {
            /*
             * aal_supports<StrictProvenance> ends up here, too, and we ensure
             * that we always allocate whole ArenaMap granules.
             */
            // if constexpr (aal_supports<StrictProvenance>)
            // {
            //   // static_assert(
            //   //   !aal_supports<StrictProvenance> ||
            //   //     (ArenaMap::alloc_size >= PAL::minimum_alloc_size),
            //   //   "Provenance root granule must be at least PAL's "
            //   //   "minimum_alloc_size");
            //   block_size = bits::align_up(size, ArenaMap::alloc_size);
            // }
            // else
            {
              /*
               * We will have handled the case where size >= minimum_alloc_size
               * above, so we are left to handle only small things here.
               */
              block_size = PAL::minimum_alloc_size;
            }

            void* block_raw = PAL::template reserve_aligned<false>(block_size);

            // It's a bit of a lie to convert without applying bounds, but the
            // platform will have bounded block for us and it's better that the
            // rest of our internals expect CBChunk bounds.
            block = CapPtr<void, CBChunk>(block_raw);

            //             if constexpr (aal_supports<StrictProvenance>)
            //             {
            //               auto root_block = CapPtr<void, CBArena>(block_raw);
            //               auto root_size = block_size;
            //               do
            //               {
            // //                arena_map.register_root(root_block);
            //                 root_block = pointer_offset(root_block,
            //                 ArenaMap::alloc_size); root_size -=
            //                 ArenaMap::alloc_size;
            //               } while (root_size > 0);
            //             }
          }
          else if constexpr (!pal_supports<NoAllocation, PAL>)
          {
            // Need at least 2 times the space to guarantee alignment.
            // Hold lock here as a race could cause additional requests to
            // the PAL, and this could lead to suprious OOM.  This is
            // particularly bad if the PAL gives all the memory on first call.
            auto block_and_size = PAL::reserve_at_least(size * 2);
            block = CapPtr<void, CBChunk>(block_and_size.first);
            block_size = block_and_size.second;

            // Ensure block is pointer aligned.
            if (
              pointer_align_up(block, sizeof(void*)) != block ||
              bits::align_up(block_size, sizeof(void*)) > block_size)
            {
              auto diff =
                pointer_diff(block, pointer_align_up(block, sizeof(void*)));
              block_size = block_size - diff;
              block_size = bits::align_down(block_size, sizeof(void*));
            }
          }
          if (block == nullptr)
          {
            return nullptr;
          }
          add_range(block, block_size);

          // still holding lock so guaranteed to succeed.
          res = remove_block(bits::next_pow2_bits(size));
        }
      }

      // Don't need lock while committing pages.
      if constexpr (committed)
        commit_block(res, size);

      return res;
    }

    /**
     * Aligns block to next power of 2 above size, and unused space at the end
     * of the block is retained by the address space manager.
     *
     * This is useful for allowing the space required for alignment to be
     * used, by smaller objects.
     */
    template<bool committed>
    CapPtr<void, CBChunk> reserve_with_left_over(size_t size)
    {
      SNMALLOC_ASSERT(size >= sizeof(void*));

      size = bits::align_up(size, sizeof(void*));

      size_t rsize = bits::next_pow2(size);

      auto res = reserve<false>(rsize);

      if (res != nullptr)
      {
        if (rsize > size)
        {
          FlagLock lock(spin_lock);
          add_range(pointer_offset(res, size), rsize - size);
        }

        if constexpr (committed)
          commit_block(res, size);
      }
      return res;
    }

    /**
     * Default constructor.  An address-space manager constructed in this way
     * does not own any memory at the start and will request any that it needs
     * from the PAL.
     */
    AddressSpaceManager() = default;

    /**
     * Constructor that pre-initialises the address-space manager with a region
     * of memory.
     */
    AddressSpaceManager(CapPtr<void, CBChunk> base, size_t length)
    {
      add_range(base, length);
    }

    /**
     * Move assignment operator.  This should only be used during initialisation
     * of the system.  There should be no concurrency.
     */
    AddressSpaceManager& operator=(AddressSpaceManager&& other) noexcept
    {
      // Lock address space manager.  This will prevent it being used by
      // mistake. Fails with deadlock with any subsequent caller.
      if (other.spin_lock.test_and_set())
        abort();
      ranges = other.ranges;
      return *this;
    }

    // ArenaMap& arenamap()
    // {
    //   return arena_map;
    // }
  };
} // namespace snmalloc
