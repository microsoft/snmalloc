#include "../ds/address.h"
#include "../ds/flaglock.h"
#include "../pal/pal.h"

#include <array>
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
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL>
  class AddressSpaceManager
  {
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
    std::array<std::array<void*, 2>, bits::BITS> ranges = {};

    /**
     * This is infrequently used code, a spin lock simplifies the code
     * considerably, and should never be on the fast path.
     */
    std::atomic_flag spin_lock = ATOMIC_FLAG_INIT;

    /**
     * Checks a block satisfies its invariant.
     */
    inline void check_block(void* base, size_t align_bits)
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
    void add_block(size_t align_bits, void* base)
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
        // Add to linked list.
        commit_block(base, sizeof(void*));
        *reinterpret_cast<void**>(base) = ranges[align_bits][1];
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
    void* remove_block(size_t align_bits)
    {
      auto first = ranges[align_bits][0];
      if (first == nullptr)
      {
        if (align_bits == (bits::BITS - 1))
        {
          // Out of memory
          return nullptr;
        }

        // Look for larger block and split up recursively
        void* bigger = remove_block(align_bits + 1);
        if (bigger != nullptr)
        {
          void* left_over =
            pointer_offset(bigger, bits::one_at_bit(align_bits));
          ranges[align_bits][0] = left_over;
          check_block(left_over, align_bits);
        }
        check_block(bigger, align_bits + 1);
        return bigger;
      }

      auto second = ranges[align_bits][1];
      if (second != nullptr)
      {
        commit_block(second, sizeof(void*));
        auto next = *reinterpret_cast<void**>(second);
        ranges[align_bits][1] = next;
        // Zero memory. Client assumes memory contains only zeros.
        *reinterpret_cast<void**>(second) = nullptr;
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
    void add_range(void* base, size_t length)
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
    void commit_block(void* base, size_t size)
    {
      // Rounding required for sub-page allocations.
      auto page_start = pointer_align_down<OS_PAGE_SIZE, char>(base);
      auto page_end =
        pointer_align_up<OS_PAGE_SIZE, char>(pointer_offset(base, size));
      PAL::template notify_using<NoZero>(
        page_start, static_cast<size_t>(page_end - page_start));
    }

  public:
    /**
     * Returns a pointer to a block of memory of the supplied size.
     * The block will be committed, if specified by the template parameter.
     * The returned block is guaranteed to be aligened to the size.
     *
     * Only request 2^n sizes, and not less than a pointer.
     */
    template<bool committed>
    void* reserve(size_t size)
    {
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(size >= sizeof(void*));

      if constexpr (pal_supports<AlignedAllocation, PAL>)
      {
        if (size >= PAL::minimum_alloc_size)
          return PAL::template reserve_aligned<committed>(size);
      }

      void* res;
      {
        FlagLock lock(spin_lock);
        res = remove_block(bits::next_pow2_bits(size));
        if (res == nullptr)
        {
          // Allocation failed ask OS for more memory
          void* block = nullptr;
          size_t block_size = 0;
          if constexpr (pal_supports<AlignedAllocation, PAL>)
          {
            block_size = PAL::minimum_alloc_size;
            block = PAL::template reserve_aligned<false>(block_size);
          }
          else if constexpr (!pal_supports<NoAllocation, PAL>)
          {
            // Need at least 2 times the space to guarantee alignment.
            // Hold lock here as a race could cause additional requests to
            // the PAL, and this could lead to suprious OOM.  This is
            // particularly bad if the PAL gives all the memory on first call.
            auto block_and_size = PAL::reserve_at_least(size * 2);
            block = block_and_size.first;
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
     * Default constructor.  An address-space manager constructed in this way
     * does not own any memory at the start and will request any that it needs
     * from the PAL.
     */
    AddressSpaceManager() = default;

    /**
     * Constructor that pre-initialises the address-space manager with a region
     * of memory.
     */
    AddressSpaceManager(void* base, size_t length)
    {
      add_range(base, length);
    }
  };
} // namespace snmalloc
