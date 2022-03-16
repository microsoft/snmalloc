#pragma once

#include "../ds/address.h"
#include "../ds/bits.h"
#include "../mem/allocconfig.h"
#include "../pal/pal.h"
#include "buddy.h"
#include "range_helpers.h"

#include <string>

namespace snmalloc
{
  /**
   * Class for using the pagemap entries for the buddy allocator.
   */
  template<SNMALLOC_CONCEPT(ConceptBackendMeta) Pagemap>
  class BuddyChunkRep
  {
  public:
    /*
     * The values we store in our rbtree are the addresses of (combined spans
     * of) chunks of the address space; as such, bits in (MIN_CHUNK_SIZE - 1)
     * are unused and so the RED_BIT is packed therein.  However, in practice,
     * these are not "just any" uintptr_t-s, but specifically the uintptr_t-s
     * inside the Pagemap's MetaEntry structures.  As such, there are some
     * additional bit-swizzling concerns; see set() and get() below.
     */
    using Holder = uintptr_t;
    using Contents = uintptr_t;

    static constexpr address_t RED_BIT = 1 << 1;

    static_assert(RED_BIT < MIN_CHUNK_SIZE);
    static_assert(RED_BIT != MetaEntry::META_BOUNDARY_BIT);
    static_assert(RED_BIT != MetaEntry::REMOTE_BACKEND_MARKER);

    static constexpr Contents null = 0;

    static void set(Holder* ptr, Contents r)
    {
      SNMALLOC_ASSERT((r & (MIN_CHUNK_SIZE - 1)) == 0);
      /*
       * Preserve lower bits, claim as backend, and update contents of holder.
       *
       * This is excessive at present but no harder than being more precise
       * while also being future-proof.  All that is strictly required would be
       * to preserve META_BOUNDARY_BIT and RED_BIT in ->meta and to assert
       * REMOTE_BACKEND_MARKER in ->remote_and_sizeclass (if it isn't already
       * asserted).  However, we don't know which Holder* we have been given,
       * nor do we know whether this Holder* is completely new (and so we are
       * the first reasonable opportunity to assert REMOTE_BACKEND_MARKER) or
       * recycled from the frontend, and so we preserve and assert more than
       * strictly necessary.
       *
       * The use of `address_cast` below is a CHERI-ism; otherwise both `r` and
       * `*ptr & ...` are plausibly provenance-carrying values and the compiler
       * balks at the ambiguity.
       */
      *ptr = r | address_cast(*ptr & (MIN_CHUNK_SIZE - 1)) |
        MetaEntry::REMOTE_BACKEND_MARKER;
    }

    static Contents get(const Holder* ptr)
    {
      return *ptr & ~(MIN_CHUNK_SIZE - 1);
    }

    static Holder& ref(bool direction, Contents k)
    {
      MetaEntry& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(k));
      if (direction)
        return entry.meta;

      return entry.remote_and_sizeclass;
    }

    static bool is_red(Contents k)
    {
      return (ref(true, k) & RED_BIT) == RED_BIT;
    }

    static void set_red(Contents k, bool new_is_red)
    {
      if (new_is_red != is_red(k))
        ref(true, k) ^= RED_BIT;
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

    static bool can_consolidate(Contents k, size_t size)
    {
      // Need to know both entries exist in the pagemap.
      // This must only be called if that has already been
      // ascertained.
      // The buddy could be in a part of the pagemap that has
      // not been registered and thus could segfault on access.
      auto larger = bits::max(k, buddy(k, size));
      MetaEntry& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(larger));
      return !entry.is_boundary();
    }
  };

  template<
    SNMALLOC_CONCEPT(ConceptBackendRange_Alloc) ParentRange,
    size_t REFILL_SIZE_BITS,
    size_t MAX_SIZE_BITS,
    SNMALLOC_CONCEPT(ConceptBackendMeta) Pagemap,
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
#ifdef __cpp_concepts
        static_assert(
          ConceptBackendRange_Dealloc<ParentRange>,
          "MAX_SIZE_BITS < address space size requires overflow to parent");
#endif
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