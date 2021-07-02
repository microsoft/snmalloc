#pragma once
#include "../ds/address.h"
#include "../ds/flaglock.h"
#include "../pal/pal.h"
#include "address_space_core.h"
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

    AddressSpaceManagerCore core;

    /**
     * This is infrequently used code, a spin lock simplifies the code
     * considerably, and should never be on the fast path.
     */
    std::atomic_flag spin_lock = ATOMIC_FLAG_INIT;

    /**
     * All memory issued by this address space manager
     */
    std::atomic<size_t> peak_memory_usage_{0};

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

      if constexpr ((align == false) && !pal_supports<NoAllocation, PAL>)
      {
        if constexpr (pal_supports<AlignedAllocation, PAL>)
          return CapPtr<void, CBChunk>(
          PAL::template reserve_aligned<committed>(size));
        else
        {
          auto [block, size2] = PAL::reserve_at_least(size);
          // TODO wasting size here.
#ifdef SNMALLOC_TRACING
          std::cout << "Unaligned alloc here:" << block << " (" << size2 << ")"
                    << std::endl;
#endif
          return CapPtr<void, CBChunk>(block);
        }
      }
      else
      {
        /*
         * For sufficiently large allocations with platforms that support
         * aligned allocations and architectures that don't require
         * StrictProvenance, try asking the platform first.
         */
        if constexpr (
          pal_supports<AlignedAllocation, PAL> &&
          !aal_supports<StrictProvenance>)
        {
          if (size >= PAL::minimum_alloc_size)
            return CapPtr<void, CBChunk>(
              PAL::template reserve_aligned<committed>(size));
        }

        CapPtr<void, CBChunk> res;
        {
          FlagLock lock(spin_lock);
          res = core.template reserve<PAL>(size);
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
                 * We will have handled the case where size >=
                 * minimum_alloc_size above, so we are left to handle only small
                 * things here.
                 */
                block_size = PAL::minimum_alloc_size;
              }

              void* block_raw =
                PAL::template reserve_aligned<false>(block_size);

              // It's a bit of a lie to convert without applying bounds, but the
              // platform will have bounded block for us and it's better that
              // the rest of our internals expect CBChunk bounds.
              block = CapPtr<void, CBChunk>(block_raw);

              //             if constexpr (aal_supports<StrictProvenance>)
              //             {
              //               auto root_block = CapPtr<void,
              //               CBArena>(block_raw); auto root_size = block_size;
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

            core.template add_range<PAL>(block, block_size);

            // still holding lock so guaranteed to succeed.
            res = core.template reserve<PAL>(size);
          }
        }

        // Don't need lock while committing pages.
        if constexpr (committed)
          core.template commit_block<PAL>(res, size);

        return res;
      }
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
          core.template add_range<PAL>(pointer_offset(res, size), rsize - size);
        }

        if constexpr (committed)
          core.commit_block<PAL>(res, size);
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
    void add_range(CapPtr<void, CBChunk> base, size_t length)
    {
      FlagLock lock(spin_lock);
      core.add_range<PAL>(base, length);
    }

    size_t peak_memory_usage()
    {
      return peak_memory_usage_;
    }

    void add_peak_memory_usage(size_t size)
    {
      peak_memory_usage_ += size;
#ifdef SNMALLOC_TRACING
      std::cout << "peak_memory_usage_: " << peak_memory_usage_ << std::endl;
#endif
    }

    // ArenaMap& arenamap()
    // {
    //   return arena_map;
    // }
  };
} // namespace snmalloc
