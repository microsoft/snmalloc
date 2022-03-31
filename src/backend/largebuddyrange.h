#pragma once

#include "../ds/address.h"
#include "../ds/bits.h"
#include "../mem/allocconfig.h"
#include "../pal/pal.h"
#include "backend_concept.h"
#include "buddy.h"
#include "range_helpers.h"

#include <string>

namespace snmalloc
{
  /**
   * Class for using the pagemap entries for the buddy allocator.
   */
  template<SNMALLOC_CONCEPT(ConceptBuddyRangeMeta) Pagemap>
  class BuddyChunkRep
  {
  public:
    /*
     * The values we store in our rbtree are the addresses of (combined spans
     * of) chunks of the address space; as such, bits in (MIN_CHUNK_SIZE - 1)
     * are unused and so the RED_BIT is packed therein.  However, in practice,
     * these are not "just any" uintptr_t-s, but specifically the uintptr_t-s
     * inside the Pagemap's MetaEntry structures.
     *
     * The MetaEntry provides us with helpers that guarantee that we use only
     * the bits that we are allowed to.
     * @{
     */
    using Holder = MetaEntryBase::BackendStateWord;
    using Contents = uintptr_t;
    ///@}

    /**
     * The bit that we will use to mark an entry as red.
     * This has constraints in two directions, it must not be one of the
     * reserved bits from the perspective of the meta entry and it must not be
     * a bit that is a valid part of the address of a chunk.
     * @{
     */
    static constexpr address_t RED_BIT = 1 << 8;

    static_assert(RED_BIT < MIN_CHUNK_SIZE);
    static_assert((RED_BIT & MetaEntryBase::BackendReservedMask) == 0);
    ///@}

    /// The value of a null node, as returned by `get`
    static constexpr Contents null = 0;
    /// The value of a null node, as stored in a `uintptr_t`.
    static constexpr Contents root = 0;

    /**
     * Set the value.  Preserve the red/black colour.
     */
    static void set(Holder ptr, Contents r)
    {
      ptr = r | (ptr.get() & RED_BIT);
    }

    /**
     * Returns the value, stripping out the red/black colour.
     */
    static Contents get(const Holder ptr)
    {
      return ptr.get() & ~RED_BIT;
    }

    /**
     * Returns a pointer to the tree node for the specified address.
     */
    static Holder ref(bool direction, Contents k)
    {
      // Special case for accessing the null entry.  We want to make sure
      // that this is never modified by the back end, so we make it point to
      // a constant entry and use the MMU to trap even in release modes.
      static const Contents null_entry = 0;
      if (SNMALLOC_UNLIKELY(address_cast(k) == 0))
      {
        return {const_cast<Contents*>(&null_entry)};
      }
      auto& entry = Pagemap::template get_metaentry_mut<false>(address_cast(k));
      if (direction)
        return entry.get_backend_word(Pagemap::Entry::Word::One);

      return entry.get_backend_word(Pagemap::Entry::Word::Two);
    }

    static bool is_red(Contents k)
    {
      return (ref(true, k).get() & RED_BIT) == RED_BIT;
    }

    static void set_red(Contents k, bool new_is_red)
    {
      if (new_is_red != is_red(k))
      {
        auto v = ref(true, k);
        v = v.get() ^ RED_BIT;
      }
      SNMALLOC_ASSERT(is_red(k) == new_is_red);
    }

    static Contents offset(Contents k, size_t size)
    {
      return k + size;
    }

    static Contents buddy(Contents k, size_t size)
    {
      return k ^ size;
    }

    static Contents align_down(Contents k, size_t size)
    {
      return k & ~(size - 1);
    }

    static bool compare(Contents k1, Contents k2)
    {
      return k1 > k2;
    }

    static bool equal(Contents k1, Contents k2)
    {
      return k1 == k2;
    }

    static uintptr_t printable(Contents k)
    {
      return k;
    }

    /**
     * Convert the pointer wrapper into something that the snmalloc debug
     * printing code can print.
     */
    static address_t printable(Holder k)
    {
      return k.printable_address();
    }

    /**
     * Returns the name for use in debugging traces.  Not used in normal builds
     * (release or debug), only when tracing is enabled.
     */
    static const char* name()
    {
      return "BuddyChunkRep";
    }

    static bool can_consolidate(Contents k, size_t size)
    {
      // Need to know both entries exist in the pagemap.
      // This must only be called if that has already been
      // ascertained.
      // The buddy could be in a part of the pagemap that has
      // not been registered and thus could segfault on access.
      auto larger = bits::max(k, buddy(k, size));
      auto& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(larger));
      return !entry.is_boundary();
    }
  };

  template<
    typename ParentRange,
    size_t REFILL_SIZE_BITS,
    size_t MAX_SIZE_BITS,
    SNMALLOC_CONCEPT(ConceptBuddyRangeMeta) Pagemap,
    bool Consolidate = true>
  class LargeBuddyRange
  {
    typename ParentRange::State parent{};

    static constexpr size_t REFILL_SIZE = bits::one_at_bit(REFILL_SIZE_BITS);

    /**
     *
     */
    Buddy<BuddyChunkRep<Pagemap>, MIN_CHUNK_BITS, MAX_SIZE_BITS> buddy_large;

    /**
     * The parent might not support deallocation if this buddy allocator covers
     * the whole range.  Uses template insanity to make this work.
     */
    template<bool exists = MAX_SIZE_BITS != (bits::BITS - 1)>
    std::enable_if_t<exists>
    parent_dealloc_range(capptr::Chunk<void> base, size_t size)
    {
      static_assert(
        MAX_SIZE_BITS != (bits::BITS - 1), "Don't set SFINAE parameter");
      parent->dealloc_range(base, size);
    }

    void dealloc_overflow(capptr::Chunk<void> overflow)
    {
      if constexpr (MAX_SIZE_BITS != (bits::BITS - 1))
      {
        if (overflow != nullptr)
        {
          parent->dealloc_range(overflow, bits::one_at_bit(MAX_SIZE_BITS));
        }
      }
      else
      {
        if (overflow != nullptr)
          abort();
      }
    }

    /**
     * Add a range of memory to the address space.
     * Divides blocks into power of two sizes with natural alignment
     */
    void add_range(capptr::Chunk<void> base, size_t length)
    {
      range_to_pow_2_blocks<MIN_CHUNK_BITS>(
        base,
        length,
        [this](capptr::Chunk<void> base, size_t align, bool first) {
          if constexpr (!Consolidate)
          {
            // Tag first entry so we don't consolidate it.
            if (first)
            {
              Pagemap::get_metaentry_mut(address_cast(base)).set_boundary();
            }
          }
          else
          {
            UNUSED(first);
          }

          auto overflow = capptr::Chunk<void>(reinterpret_cast<void*>(
            buddy_large.add_block(base.unsafe_uintptr(), align)));

          dealloc_overflow(overflow);
        });
    }

    capptr::Chunk<void> refill(size_t size)
    {
      if (ParentRange::Aligned)
      {
        // TODO have to add consolidation blocker for these cases.
        if (size >= REFILL_SIZE)
        {
          return parent->alloc_range(size);
        }

        auto refill_range = parent->alloc_range(REFILL_SIZE);
        if (refill_range != nullptr)
          add_range(pointer_offset(refill_range, size), REFILL_SIZE - size);
        return refill_range;
      }

      // Need to overallocate to get the alignment right.
      bool overflow = false;
      size_t needed_size = bits::umul(size, 2, overflow);
      if (overflow)
      {
        return nullptr;
      }

      auto refill_size = bits::max(needed_size, REFILL_SIZE);
      while (needed_size <= refill_size)
      {
        auto refill = parent->alloc_range(refill_size);

        if (refill != nullptr)
        {
          add_range(refill, refill_size);

          SNMALLOC_ASSERT(refill_size < bits::one_at_bit(MAX_SIZE_BITS));
          static_assert(
            (REFILL_SIZE < bits::one_at_bit(MAX_SIZE_BITS)) ||
              ParentRange::Aligned,
            "Required to prevent overflow.");

          return alloc_range(size);
        }

        refill_size >>= 1;
      }

      return nullptr;
    }

  public:
    class State
    {
      LargeBuddyRange buddy_range;

    public:
      LargeBuddyRange* operator->()
      {
        return &buddy_range;
      }

      constexpr State() = default;
    };

    static constexpr bool Aligned = true;

    static constexpr bool ConcurrencySafe = false;

    constexpr LargeBuddyRange() = default;

    capptr::Chunk<void> alloc_range(size_t size)
    {
      SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);
      SNMALLOC_ASSERT(bits::is_pow2(size));

      if (size >= (bits::one_at_bit(MAX_SIZE_BITS) - 1))
      {
        if (ParentRange::Aligned)
          return parent->alloc_range(size);

        return nullptr;
      }

      auto result = capptr::Chunk<void>(
        reinterpret_cast<void*>(buddy_large.remove_block(size)));

      if (result != nullptr)
        return result;

      return refill(size);
    }

    void dealloc_range(capptr::Chunk<void> base, size_t size)
    {
      SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);
      SNMALLOC_ASSERT(bits::is_pow2(size));

      if constexpr (MAX_SIZE_BITS != (bits::BITS - 1))
      {
        if (size >= (bits::one_at_bit(MAX_SIZE_BITS) - 1))
        {
          parent_dealloc_range(base, size);
          return;
        }
      }

      auto overflow = capptr::Chunk<void>(reinterpret_cast<void*>(
        buddy_large.add_block(base.unsafe_uintptr(), size)));
      dealloc_overflow(overflow);
    }
  };
} // namespace snmalloc
