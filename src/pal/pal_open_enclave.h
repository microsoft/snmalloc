#pragma once

#include "ds/address.h"
#include "ds/flaglock.h"
#include "pal_plain.h"

#include <array>
#ifdef OPEN_ENCLAVE
extern "C" void* oe_memset_s(void* p, size_t p_size, int c, size_t size);
extern "C" [[noreturn]] void oe_abort();

namespace snmalloc
{
  class PALOpenEnclave
  {
    /**
     * Implements a power of two allocator, where all blocks are aligned to the
     * same power of two as their size. This is what snmalloc uses to get
     * alignment of very large sizeclasses.
     *
     * Pals are not required to unreserve memory, so this does not require the
     * usual complexity of a buddy allocator.
     */

    // There are a maximum of two blocks for any size/align in a range.
    // One before the point of maximum alignment, and one after.
    static inline std::array<std::array<void*, 2>, bits::BITS> ranges;
    // This is infrequently used code, a spin lock simplifies the code
    // considerably, and should never be on the fast path.
    static inline std::atomic_flag spin_lock;

    static void add_block(size_t align_bits, void* base)
    {
      if (ranges[align_bits][0] == nullptr)
      {
        ranges[align_bits][0] = base;
        return;
      }

      if (ranges[align_bits][1] != nullptr)
        error("Critical assumption violated!");

      ranges[align_bits][1] = base;
    }

    static void* remove_block(size_t align_bits)
    {
      auto first = ranges[align_bits][0];
      if (first == nullptr)
      {
        if (align_bits < (bits::BITS - 1))
        {
          // Look for larger block and split up recursively
          void* bigger = remove_block(align_bits + 1);
          if (bigger == nullptr)
          {
            // Out of memory.
            return bigger;
          }
          void* left_over =
            pointer_offset(bigger, bits::one_at_bit(align_bits));
          ranges[align_bits][0] = left_over;
          return bigger;
        }
        // Out of memory
        return nullptr;
      }

      auto second = ranges[align_bits][1];
      if (second != nullptr)
      {
        ranges[align_bits][1] = nullptr;
        return second;
      }

      ranges[align_bits][0] = nullptr;
      return first;
    }

  public:
    /**
     * This will be called by oe_allocator_init to set up enclave heap bounds.
     */
    static void setup_initial_range(void* base, void* end)
    {
      // Find the minimum set of maximally aligned blocks in this range.
      // Each block's alignment and size are equal.
      size_t length = pointer_diff(base, end);
      while (length != 0)
      {
        size_t base_align_bits = bits::ctz(address_cast(base));
        size_t length_align_bits = (bits::BITS - 1) - bits::clz(length);
        size_t align_bits = bits::min(base_align_bits, length_align_bits);
        size_t align = bits::one_at_bit(align_bits);

        add_block(align_bits, base);

        base = pointer_offset(base, align);
        length -= align;
      }
    }

    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     */
    static constexpr uint64_t pal_features = AlignedAllocation;

    static constexpr size_t page_size = 0x1000;

    [[noreturn]] static void error(const char* const str)
    {
      UNUSED(str);
      oe_abort();
    }

    template<bool committed>
    static void* reserve(size_t size, size_t align) noexcept
    {
      // The following are all true from the current way snmalloc uses the PAL.
      // The implementation here is depending on them.
      SNMALLOC_ASSERT(size == bits::next_pow2(size));
      SNMALLOC_ASSERT(align == bits::next_pow2(align));
      if (size != align)
        error("Critical assumption violated!");

      FlagLock lock(spin_lock);
      size_t align_bits = bits::next_pow2_bits(align);
      return remove_block(align_bits);
    }

    template<bool page_aligned = false>
    void zero(void* p, size_t size) noexcept
    {
      oe_memset_s(p, size, 0, size);
    }
  };
}
#endif
