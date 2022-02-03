#pragma once
#include "../ds/address.h"
#include "../ds/flaglock.h"
#include "../pal/pal.h"
#include "address_space_core.h"

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
  template<
    SNMALLOC_CONCEPT(ConceptPAL) PAL,
    SNMALLOC_CONCEPT(ConceptBackendMetaRange) Pagemap>
  class AddressSpaceManager
  {
    AddressSpaceManagerCore<Pagemap> core;

    /**
     * This is infrequently used code, a spin lock simplifies the code
     * considerably, and should never be on the fast path.
     */
    FlagWord spin_lock{};

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
    template<bool committed>
    capptr::Chunk<void>
    reserve(typename Pagemap::LocalState* local_state, size_t size)
    {
#ifdef SNMALLOC_TRACING
      std::cout << "ASM reserve request:" << size << std::endl;
#endif
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(size >= sizeof(void*));

      /*
       * For sufficiently large allocations with platforms that support aligned
       * allocations, try asking the platform directly.
       */
      if constexpr (pal_supports<AlignedAllocation, PAL>)
      {
        if (size >= PAL::minimum_alloc_size)
        {
          auto base =
            capptr::Chunk<void>(PAL::template reserve_aligned<committed>(size));
          Pagemap::register_range(local_state, address_cast(base), size);
          return base;
        }
      }

      capptr::Chunk<void> res;
      {
        FlagLock lock(spin_lock);
        res = core.template reserve<PAL>(local_state, size);
        if (res == nullptr)
        {
          // Allocation failed ask OS for more memory
          capptr::Chunk<void> block = nullptr;
          size_t block_size = 0;
          if constexpr (pal_supports<AlignedAllocation, PAL>)
          {
            /*
             * We will have handled the case where size >=
             * minimum_alloc_size above, so we are left to handle only small
             * things here.
             */
            block_size = PAL::minimum_alloc_size;

            void* block_raw = PAL::template reserve_aligned<false>(block_size);

            // It's a bit of a lie to convert without applying bounds, but the
            // platform will have bounded block for us and it's better that
            // the rest of our internals expect CBChunk bounds.
            block = capptr::Chunk<void>(block_raw);
          }
          else if constexpr (!pal_supports<NoAllocation, PAL>)
          {
            // Need at least 2 times the space to guarantee alignment.
            bool overflow;
            size_t needed_size = bits::umul(size, 2, overflow);
            if (overflow)
            {
              return nullptr;
            }
            // Magic number (27) for over-allocating a block of memory
            // These should be further refined based on experiments.
            constexpr size_t min_size = bits::one_at_bit(27);
            for (size_t size_request = bits::max(needed_size, min_size);
                 size_request >= needed_size;
                 size_request = size_request / 2)
            {
              block = capptr::Chunk<void>(PAL::reserve(size_request));
              if (block != nullptr)
              {
                block_size = size_request;
                break;
              }
            }

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

          Pagemap::register_range(local_state, address_cast(block), block_size);

          core.template add_range<PAL>(local_state, block, block_size);

          // still holding lock so guaranteed to succeed.
          res = core.template reserve<PAL>(local_state, size);
        }
      }

      // Don't need lock while committing pages.
      if constexpr (committed)
        core.template commit_block<PAL>(res, size);

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
    capptr::Chunk<void> reserve_with_left_over(
      typename Pagemap::LocalState* local_state, size_t size)
    {
      SNMALLOC_ASSERT(size >= sizeof(void*));

      size = bits::align_up(size, sizeof(void*));

      size_t rsize = bits::next_pow2(size);

      auto res = reserve<false>(local_state, rsize);

      if (res != nullptr)
      {
        if (rsize > size)
        {
          FlagLock lock(spin_lock);
          core.template add_range<PAL>(
            local_state, pointer_offset(res, size), rsize - size);
        }

        if constexpr (committed)
          core.template commit_block<PAL>(res, size);
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
     * Add a range of memory to the address space.
     * Divides blocks into power of two sizes with natural alignment
     */
    void add_range(
      typename Pagemap::LocalState* local_state,
      capptr::Chunk<void> base,
      size_t length)
    {
      FlagLock lock(spin_lock);
      core.template add_range<PAL>(local_state, base, length);
    }
  };
} // namespace snmalloc
