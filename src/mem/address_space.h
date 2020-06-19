#include "../ds/address.h"
#include "../ds/flaglock.h"
#include "../pal/pal.h"

#include <array>
#include <iostream>
namespace snmalloc
{
  template<typename Pal>
  class AddressSpaceManager : public Pal
  {
    /**
     * Implements a power of two allocator, where all blocks are aligned to the
     * same power of two as their size. This is what snmalloc uses to get
     * alignment of very large sizeclasses.
     *
     * It cannot unreserve memory, so this does not require the
     * usual complexity of a buddy allocator.
     */

    // There are a maximum of two blocks for any size/align in a range.
    // One before the point of maximum alignment, and one after.
    // As we can add multiple ranges the second entry's block may contain
    // a pointer to subsequent blocks.
    std::array<std::array<void*, 2>, bits::BITS> ranges = {};

    // This is infrequently used code, a spin lock simplifies the code
    // considerably, and should never be on the fast path.
    std::atomic_flag spin_lock = ATOMIC_FLAG_INIT;

    inline void check_block(void* base, size_t align_bits)
    {
      SNMALLOC_ASSERT(
        base == pointer_align_up(base, bits::one_at_bit(align_bits)));
      // All blocks need to be bigger than a pointer.
      SNMALLOC_ASSERT(bits::one_at_bit(align_bits) >= sizeof(void*));
      UNUSED(base);
      UNUSED(align_bits);
    }

    void add_block(size_t align_bits, void* base)
    {
      check_block(base, align_bits);
      if (ranges[align_bits][0] == nullptr)
      {
        ranges[align_bits][0] = base;
        return;
      }

      if (ranges[align_bits][1] != nullptr)
      {
        commit_first_page(base);
        *(void**)base = ranges[align_bits][1];
        check_block(ranges[align_bits][1], align_bits);
      }

      ranges[align_bits][1] = base;
      check_block(ranges[align_bits][1], align_bits);
    }

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
        commit_first_page(second);
        auto next = *(void**)second;
        ranges[align_bits][1] = next;
        // Zero memory. Client assumes memory contains only zeros.
        *(void**)second = nullptr;
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

    void commit_block(void* base, size_t size)
    {
      auto page_start = pointer_align_down<OS_PAGE_SIZE, char>(base);
      auto page_end =
        pointer_align_up<OS_PAGE_SIZE, char>(pointer_offset(base, size));
      Pal::template notify_using<NoZero>(
        page_start, static_cast<size_t>(page_end - page_start));
    }

    void commit_first_page(void* base)
    {
      auto page_start = pointer_align_down<OS_PAGE_SIZE, char>(base);
      Pal::template notify_using<NoZero>(page_start, OS_PAGE_SIZE);
    }

  public:
    template<bool committed>
    void* reserve(size_t size)
    {
      SNMALLOC_ASSERT(bits::next_pow2(size) == size);
      SNMALLOC_ASSERT(size >= sizeof(void*));

      if constexpr (pal_supports<AlignedAllocation, Pal>)
      {
        if (size >= Pal::minimum_alloc_size)
          return static_cast<Pal*>(this)->template reserve_aligned<committed>(size);
      }

      void* res;
      {
        FlagLock lock(spin_lock);
        res = remove_block(bits::next_pow2_bits(size));
        if (res == nullptr)
        {
          // Allocation failed ask OS for more memory
          void* block;
          size_t block_size;
          if constexpr (pal_supports<AlignedAllocation, Pal>)
          {
            block_size = Pal::minimum_alloc_size;
            block = static_cast<Pal*>(this)->template reserve_aligned<false>(block_size);
          }
          else
          {
            // Need at least 2 times the space to guarantee alignment.
            // Hold lock here incase Pal only provides a single range of memory.
            auto block_and_size = static_cast<Pal*>(this)->reserve_at_least(size * 2);
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
  };
}