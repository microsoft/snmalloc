#pragma once

#include "arena.h"
#include "empty_range.h"
#include "range_helpers.h"

namespace snmalloc
{
  /**
   * PagemapRep — Rep for `Arena` over a Pagemap.
   *
   * Each free block uses three pagemap entries at unit-aligned offsets:
   *
   *   Unit 0 (addr):                bin-tree node + variant tag.
   *   Unit 1 (addr + UNIT_SIZE):    range-tree node (size ≥ 2 units).
   *   Unit 2 (addr + 2*UNIT_SIZE):  large chunk count (size ≥ 3 units).
   *
   * Bit-layout decisions for tree nodes are private to this class:
   * - Bits 0–7 of each pagemap word are reserved by the pagemap.
   * - Bit 8 is the red bit (both trees).
   * - Bits 9–10 of Word::One at unit 0 hold the variant tag.
   * - Large chunk count is stored shifted left by 8 in Word::One of
   *   unit 2.
   *
   * `MIN_SIZE_BITS` is the log2 size of the allocation unit (= pagemap
   * stride); the caller passes whatever unit it uses (snmalloc's global
   * `MIN_CHUNK_BITS` in the in-tree pipeline).
   * `MAX_SIZE_BITS` is the log2 of the (exclusive) upper bound on block
   * size in bytes; used here only to verify that the largest chunk
   * count fits in a shifted pagemap word.
   */
  template<
    SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap,
    size_t MIN_SIZE_BITS,
    size_t MAX_SIZE_BITS>
  class PagemapRep
  {
    using Entry = typename Pagemap::Entry;

    static constexpr uintptr_t UNIT_SIZE = uintptr_t(1) << MIN_SIZE_BITS;

    // Bit positions inside a pagemap word. Bits 0–7 are reserved by the
    // pagemap; tree-node and large-size encodings start at bit 8.
    static constexpr unsigned RED_BIT_POS = 8;
    static constexpr unsigned VARIANT_SHIFT = 9;
    static constexpr unsigned VARIANT_BITS = 2;

    // Shift used to encode the large-size chunk count in Word::One of
    // unit 2.
    static constexpr size_t LARGE_SIZE_SHIFT = 8;

    static constexpr uintptr_t RED_BIT = uintptr_t(1) << RED_BIT_POS;
    static constexpr uintptr_t VARIANT_MASK =
      ((uintptr_t(1) << VARIANT_BITS) - 1) << VARIANT_SHIFT;
    static constexpr uintptr_t BIN_META_MASK = RED_BIT | VARIANT_MASK;
    static constexpr uintptr_t RANGE_META_MASK = RED_BIT;

    static_assert(MAX_SIZE_BITS > MIN_SIZE_BITS);
    static_assert(
      (MAX_SIZE_BITS - MIN_SIZE_BITS) + LARGE_SIZE_SHIFT <= bits::BITS,
      "Shifted large-size field must fit in a pagemap word.");
    static_assert((RED_BIT & VARIANT_MASK) == 0);
    static_assert(BIN_META_MASK < UNIT_SIZE);
    static_assert(
      Entry::is_backend_allowed_value(Entry::Word::One, BIN_META_MASK));
    static_assert(Entry::is_backend_allowed_value(Entry::Word::Two, RED_BIT));

    using Word = typename Entry::Word;
    using Handle = typename Entry::BackendStateWordRef;

    /**
     * Pagemap word for the `UnitIdx`-th unit of the block at `addr`.
     * Centralises the layout decision "which pagemap entry encodes
     * data for unit i". Used by `TreeRep::ref` and by the variant /
     * large-size accessors below.
     */
    template<size_t UnitIdx>
    static Handle word_at(uintptr_t addr, Word w)
    {
      auto& entry = Pagemap::template get_metaentry_mut<false>(
        address_cast(addr + UnitIdx * UNIT_SIZE));
      return entry.get_backend_word(w);
    }

    /**
     * RBTree Rep shared by `BinRep` and `RangeRep`. `UnitIdx` selects
     * which unit (0 or 1) of the block holds this Rep's tree node; the
     * Rep's pagemap words live at `addr + UnitIdx * UNIT_SIZE`.
     * `MetaMask` covers the bits in that node's words that are owned by
     * this Rep (red + any tag bits) and must be preserved by get/set.
     */
    template<size_t UnitIdx, uintptr_t MetaMask, const char* Name>
    struct TreeRep
    {
      using Handle = PagemapRep::Handle;
      using Contents = uintptr_t;

      static constexpr Contents null = 0;
      static constexpr Contents root = 0;

      static Handle ref(bool direction, Contents k)
      {
        static const Contents null_entry = 0;
        if (SNMALLOC_UNLIKELY(k == 0))
          return Handle{const_cast<Contents*>(&null_entry)};
        return word_at<UnitIdx>(k, direction ? Word::One : Word::Two);
      }

      static Contents get(Handle h)
      {
        return h.get() & ~MetaMask;
      }

      static void set(Handle h, Contents v)
      {
        h = v | (h.get() & MetaMask);
      }

      static bool is_red(Contents k)
      {
        return (ref(true, k).get() & RED_BIT) == RED_BIT;
      }

      static void set_red(Contents k, bool new_is_red)
      {
        if (new_is_red != is_red(k))
        {
          auto h = ref(true, k);
          h = h.get() ^ RED_BIT;
        }
        SNMALLOC_ASSERT(is_red(k) == new_is_red);
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

      static uintptr_t printable(Handle h)
      {
        return h.printable_address();
      }

      static const char* name()
      {
        return Name;
      }
    };

    static constexpr char BIN_REP_NAME[] = "PagemapBinRep";
    static constexpr char RANGE_REP_NAME[] = "PagemapRangeRep";

  public:
    using BinRep = TreeRep<0, BIN_META_MASK, BIN_REP_NAME>;
    using RangeRep = TreeRep<1, RANGE_META_MASK, RANGE_REP_NAME>;

    static ArenaVariant get_variant(uintptr_t addr)
    {
      auto w = word_at<0>(addr, Word::One);
      return static_cast<ArenaVariant>(
        (w.get() & VARIANT_MASK) >> VARIANT_SHIFT);
    }

    static void set_variant(uintptr_t addr, ArenaVariant v)
    {
      auto w = word_at<0>(addr, Word::One);
      w = (w.get() & ~VARIANT_MASK) |
        (static_cast<uintptr_t>(v) << VARIANT_SHIFT);
    }

    static size_t get_large_size(uintptr_t addr)
    {
      // Stored as chunk count to keep the shifted field within a
      // pagemap word (see LARGE_SIZE_SHIFT static_assert). Returns
      // the byte size.
      return (word_at<2>(addr, Word::One).get() >> LARGE_SIZE_SHIFT)
        << MIN_SIZE_BITS;
    }

    static void set_large_size(uintptr_t addr, size_t size)
    {
      SNMALLOC_ASSERT((size & (UNIT_SIZE - 1)) == 0);
      word_at<2>(addr, Word::One) = (size >> MIN_SIZE_BITS) << LARGE_SIZE_SHIFT;
    }

    static bool can_consolidate(uintptr_t higher_addr)
    {
      auto& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(higher_addr));
      return !entry.is_boundary();
    }
  };

  /**
   * Range wrapper around Arena, presenting the standard
   * Range interface for use in Pipe<...> compositions.
   */
  template<
    size_t REFILL_SIZE_BITS,
    size_t MAX_SIZE_BITS,
    SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap,
    size_t MIN_REFILL_SIZE_BITS = 0>
  class LargeArenaRange
  {
    static_assert(
      REFILL_SIZE_BITS <= MAX_SIZE_BITS, "REFILL_SIZE_BITS > MAX_SIZE_BITS");
    static_assert(
      MIN_REFILL_SIZE_BITS <= REFILL_SIZE_BITS,
      "MIN_REFILL_SIZE_BITS > REFILL_SIZE_BITS");

    static constexpr size_t REFILL_SIZE = bits::one_at_bit(REFILL_SIZE_BITS);
    static constexpr size_t MIN_REFILL_SIZE =
      bits::one_at_bit(MIN_REFILL_SIZE_BITS);

  public:
    template<typename ParentRange = EmptyRange<>>
    class Type : public ContainsParent<ParentRange>
    {
      using ContainsParent<ParentRange>::parent;

      using PagemapRepT = PagemapRep<Pagemap, MIN_CHUNK_BITS, MAX_SIZE_BITS>;

      Arena<PagemapRepT, MIN_CHUNK_BITS, MAX_SIZE_BITS> arena;
      size_t requested_total = 0;

      void parent_dealloc(uintptr_t addr, size_t size)
      {
        if constexpr (MAX_SIZE_BITS != (bits::BITS - 1))
        {
          auto base =
            capptr::Arena<void>::unsafe_from(reinterpret_cast<void*>(addr));
          parent.dealloc_range(base, size);
        }
        else
        {
          SNMALLOC_CHECK_MSG(false, "Global range overflow should not happen");
        }
      }

      void add_range(capptr::Arena<void> base, size_t length)
      {
        // Parent ranges (e.g. mmap-backed PalRange) may return regions
        // that are page-aligned but not chunk-aligned; trim to chunk
        // boundaries on both ends before handing to the arena.
        uintptr_t lo = bits::align_up(base.unsafe_uintptr(), MIN_CHUNK_SIZE);
        uintptr_t hi =
          bits::align_down(base.unsafe_uintptr() + length, MIN_CHUNK_SIZE);
        if (lo >= hi)
          return;
        auto [ov_addr, ov_size] = arena.add_block(lo, hi - lo);
        if (ov_addr != 0)
          parent_dealloc(ov_addr, ov_size);
      }

      capptr::Arena<void> refill(size_t size)
      {
        if (ParentRange::Aligned)
        {
          size_t refill_size = bits::min(REFILL_SIZE, requested_total);
          refill_size = bits::max(refill_size, MIN_REFILL_SIZE);
          refill_size = bits::max(refill_size, size);
          refill_size = bits::next_pow2(refill_size);

          auto refill_range = parent.alloc_range(refill_size);
          if (refill_range != nullptr)
          {
            requested_total += refill_size;
            add_range(pointer_offset(refill_range, size), refill_size - size);
          }
          return refill_range;
        }

        bool overflow = false;
        size_t needed_size = bits::umul(size, 2, overflow);
        if (overflow)
        {
          return nullptr;
        }

        auto refill_size = bits::max(needed_size, REFILL_SIZE);
        while (needed_size <= refill_size)
        {
          auto refill_range = parent.alloc_range(refill_size);

          if (refill_range != nullptr)
          {
            requested_total += refill_size;
            add_range(refill_range, refill_size);

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
      static constexpr bool Aligned = true;
      static constexpr bool ConcurrencySafe = false;
      using ChunkBounds = capptr::bounds::Arena;
      static_assert(
        stl::is_same_v<typename ParentRange::ChunkBounds, ChunkBounds>);

      constexpr Type() = default;

      /**
       * `size` exceeds the arena's representable range and must be
       * routed to the parent (or refused if no parent exists). Matches
       * `Arena::add_block`'s `size < bits::one_at_bit(MAX_SIZE_BITS)`
       * precondition exactly, so alloc and dealloc bypass on the same
       * boundary.
       */
      static constexpr bool is_too_large(size_t size)
      {
        return size >= bits::one_at_bit(MAX_SIZE_BITS);
      }

      capptr::Arena<void> alloc_range(size_t size)
      {
        SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);
        SNMALLOC_ASSERT((size & (MIN_CHUNK_SIZE - 1)) == 0);

        if (is_too_large(size))
        {
          if (ParentRange::Aligned)
            return parent.alloc_range(size);

          return nullptr;
        }

        uintptr_t addr = arena.remove_block(size);
        if (addr != 0)
        {
          return capptr::Arena<void>::unsafe_from(
            reinterpret_cast<void*>(addr));
        }

        return refill(size);
      }

      void dealloc_range(capptr::Arena<void> base, size_t size)
      {
        SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);
        SNMALLOC_ASSERT((size & (MIN_CHUNK_SIZE - 1)) == 0);

        if constexpr (MAX_SIZE_BITS != (bits::BITS - 1))
        {
          if (is_too_large(size))
          {
            parent_dealloc(base.unsafe_uintptr(), size);
            return;
          }
        }

        auto [ov_addr, ov_size] =
          arena.add_block(base.unsafe_uintptr(), size);
        if (ov_addr != 0)
          parent_dealloc(ov_addr, ov_size);
      }
    };
  };
} // namespace snmalloc
