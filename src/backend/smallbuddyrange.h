#pragma once

#include "../ds/address.h"
#include "../pal/pal.h"
#include "range_helpers.h"

namespace snmalloc
{
  /**
   * struct for representing the redblack nodes
   * directly inside the meta data.
   */
  struct FreeChunk
  {
    capptr::Chunk<FreeChunk> left;
    capptr::Chunk<FreeChunk> right;
  };

  /**
   * Class for using the allocations own space to store in the RBTree.
   */
  class BuddyInplaceRep
  {
  public:
    using Holder = capptr::Chunk<FreeChunk>;
    using Contents = capptr::Chunk<FreeChunk>;

    static constexpr Contents null = nullptr;

    static constexpr address_t MASK = 1;
    static void set(Holder* ptr, Contents r)
    {
      SNMALLOC_ASSERT((address_cast(r) & MASK) == 0);
      if (r == nullptr)
        *ptr = capptr::Chunk<FreeChunk>(
          reinterpret_cast<FreeChunk*>((*ptr).unsafe_uintptr() & MASK));
      else
        // Preserve lower bit.
        *ptr = pointer_offset(r, (address_cast(*ptr) & MASK))
                 .template as_static<FreeChunk>();
    }

    static Contents get(Holder* ptr)
    {
      return pointer_align_down<2, FreeChunk>((*ptr).as_void());
    }

    static Holder& ref(bool direction, Contents r)
    {
      if (direction)
        return r->left;

      return r->right;
    }

    static bool is_red(Contents k)
    {
      if (k == nullptr)
        return false;
      return (address_cast(ref(false, k)) & MASK) == MASK;
    }

    static void set_red(Contents k, bool new_is_red)
    {
      if (new_is_red != is_red(k))
      {
        auto& r = ref(false, k);
        auto old_addr = pointer_align_down<2, FreeChunk>(r.as_void());

        if (new_is_red)
        {
          if (old_addr == nullptr)
            r = capptr::Chunk<FreeChunk>(reinterpret_cast<FreeChunk*>(MASK));
          else
            r = pointer_offset(old_addr, MASK).template as_static<FreeChunk>();
        }
        else
        {
          r = old_addr;
        }
      }
    }

    static Contents offset(Contents k, size_t size)
    {
      return pointer_offset(k, size).template as_static<FreeChunk>();
    }

    static Contents buddy(Contents k, size_t size)
    {
      // This is just doing xor size, but with what API
      // exists on capptr.
      auto base = pointer_align_down<FreeChunk>(k.as_void(), size * 2);
      auto offset = (address_cast(k) & size) ^ size;
      return pointer_offset(base, offset).template as_static<FreeChunk>();
    }

    static Contents align_down(Contents k, size_t size)
    {
      return pointer_align_down<FreeChunk>(k.as_void(), size);
    }

    static bool compare(Contents k1, Contents k2)
    {
      return address_cast(k1) > address_cast(k2);
    }

    static bool equal(Contents k1, Contents k2)
    {
      return address_cast(k1) == address_cast(k2);
    }

    static address_t printable(Contents k)
    {
      return address_cast(k);
    }

    static bool can_consolidate(Contents k, size_t size)
    {
      UNUSED(k, size);
      return true;
    }
  };

  template<typename ParentRange>
  class SmallBuddyRange
  {
    typename ParentRange::State parent{};

    static constexpr size_t MIN_BITS =
      bits::next_pow2_bits_const(sizeof(FreeChunk));

    Buddy<BuddyInplaceRep, MIN_BITS, MIN_CHUNK_BITS> buddy_small;

    /**
     * Add a range of memory to the address space.
     * Divides blocks into power of two sizes with natural alignment
     */
    void add_range(capptr::Chunk<void> base, size_t length)
    {
      range_to_pow_2_blocks<MIN_BITS>(
        base, length, [this](capptr::Chunk<void> base, size_t align, bool) {
          capptr::Chunk<void> overflow =
            buddy_small.add_block(base.as_reinterpret<FreeChunk>(), align)
              .template as_reinterpret<void>();
          if (overflow != nullptr)
            parent->dealloc_range(overflow, bits::one_at_bit(MIN_CHUNK_BITS));
        });
    }

    capptr::Chunk<void> refill(size_t size)
    {
      auto refill = parent->alloc_range(MIN_CHUNK_SIZE);

      if (refill != nullptr)
        add_range(pointer_offset(refill, size), MIN_CHUNK_SIZE - size);

      return refill;
    }

  public:
    class State
    {
      SmallBuddyRange buddy_range;

    public:
      SmallBuddyRange* operator->()
      {
        return &buddy_range;
      }

      constexpr State() = default;
    };

    static constexpr bool Aligned = true;
    static_assert(ParentRange::Aligned, "ParentRange must be aligned");

    constexpr SmallBuddyRange() = default;

    capptr::Chunk<void> alloc_range(size_t size)
    {
      if (size >= MIN_CHUNK_SIZE)
      {
        return parent->alloc_range(size);
      }

      auto result = buddy_small.remove_block(size);
      if (result != nullptr)
      {
        result->left = nullptr;
        result->right = nullptr;
        return result.template as_reinterpret<void>();
      }
      return refill(size);
    }

    capptr::Chunk<void> alloc_range_with_leftover(size_t size)
    {
      SNMALLOC_ASSERT(size <= MIN_CHUNK_SIZE);

      auto rsize = bits::next_pow2(size);

      auto result = alloc_range(rsize);

      if (result == nullptr)
        return nullptr;

      auto remnant = pointer_offset(result, size);

      add_range(remnant, rsize - size);

      return result.template as_reinterpret<void>();
    }

    void dealloc_range(capptr::Chunk<void> base, size_t size)
    {
      if (size >= MIN_CHUNK_SIZE)
      {
        parent->dealloc_range(base, size);
        return;
      }

      add_range(base, size);
    }
  };
} // namespace snmalloc