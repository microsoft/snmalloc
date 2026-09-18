#pragma once

#include "../ds_core/bits.h"
#include "../ds_core/defines.h"
#include "../ds_core/sizeclassconfig.h"
#include "arena.h"

#include <stdint.h>

namespace snmalloc
{
  /**
   * In-band tree node stored at the head of a free block managed by
   * `Arena`. Two pointer-sized words per unit; bit-packing of
   * red and variant tags lives in `word_one`. Stored as `uintptr_t`
   * so we can OR meta bits into the pointer slot without UB on
   * non-capability platforms (on CHERI, capabilities to access these
   * words are re-derived from the `Authmap` — see `InplaceRep`).
   */
  template<SNMALLOC_CONCEPT(capptr::IsBound) bounds>
  struct InplaceNode
  {
    uintptr_t word_one;
    uintptr_t word_two;
  };

  /**
   * In-band `Rep` for `Arena`. Each free block carries its
   * own tree-node and metadata storage in its first few units:
   *
   *   Unit 0 (addr):                bin-tree node + variant tag.
   *   Unit 1 (addr + UNIT_SIZE):    range-tree node (size >= 2 units).
   *   Unit 2 (addr + 2*UNIT_SIZE):  large-size word (size >= 3 units).
   *
   * Bit layout in `word_one` of each unit:
   *   bit 0           : red bit (both trees)
   *   bits 1..2       : variant tag (`ArenaVariant`, unit 0 only)
   * `word_two` holds the second child pointer with no packed meta.
   * Both child pointers are unit-aligned, so their low `MIN_BITS`
   * bits are zero — the packed meta occupies bits below
   * `1 << MIN_BITS` and never collides with a stored pointer value.
   *
   * `MIN_BITS = next_pow2_bits_const(sizeof(InplaceNode))`: the
   * smallest free block must hold one tree node, so the unit IS the
   * node footprint rounded up.
   *
   * CHERI: in-band storage is accessed via
   * `Authmap::amplify_from_address(addr)`, which returns a
   * capability at `addr` with the registered arena's permissions.
   * The authmap is set once per arena registration and never
   * mutated, so this lookup carries no concurrency hazard. On
   * non-CHERI platforms the authmap is the pass-through
   * `DummyAuthmap` and the cap collapses to a raw pointer.
   */
  template<typename Authmap, SNMALLOC_CONCEPT(capptr::IsBound) bounds>
  class InplaceRep
  {
  public:
    static constexpr size_t MIN_BITS =
      bits::next_pow2_bits_const(sizeof(InplaceNode<bounds>));
    static constexpr size_t UNIT_SIZE = size_t(1) << MIN_BITS;

    // 3 meta bits (variant 2 + red 1) packed below the unit
    // alignment boundary. Block addresses are UNIT_SIZE-aligned, so
    // a value v with `(v & (UNIT_SIZE - 1)) == 0` writes the
    // pointer cleanly without touching meta.
    static_assert(MIN_BITS >= 3, "Need 3 low bits for red+variant packing");
    static_assert(MIN_BITS < MIN_CHUNK_BITS, "Arena needs a non-trivial range");
    static_assert(
      MIN_ALLOC_SIZE >= (size_t(1) << MIN_BITS),
      "Front-end minimum allocation must be >= in-band unit size; "
      "otherwise a free block cannot hold the tree node.");

    static constexpr uintptr_t RED_BIT = 1;
    static constexpr unsigned VARIANT_SHIFT = 1;
    static constexpr unsigned VARIANT_BITS = 2;
    static constexpr uintptr_t VARIANT_MASK =
      ((uintptr_t(1) << VARIANT_BITS) - 1) << VARIANT_SHIFT;
    static constexpr uintptr_t BIN_META_MASK = RED_BIT | VARIANT_MASK;
    static constexpr uintptr_t RANGE_META_MASK = RED_BIT;

    static_assert(BIN_META_MASK < UNIT_SIZE);

    /**
     * Wraps a `uintptr_t*` storage slot plus the meta-bit mask that
     * this slot owns. `get()` returns the slot value with meta bits
     * cleared; assignment preserves them. Mirrors the role of
     * `BackendStateWordRef` but with an inline mask field (we own
     * the only mask here, unlike `BackendStateWordRef` which layers
     * on top of the frontend-reserved mask).
     */
    class Handle
    {
      uintptr_t* val{nullptr};
      uintptr_t mask{0};

    public:
      constexpr Handle() = default;

      constexpr Handle(uintptr_t* v, uintptr_t m) : val(v), mask(m) {}

      /**
       * Single-pointer constructor required by the `RBRepMethods`
       * concept (`ds_core/redblacktree.h:64-67`) for sentinel
       * construction from `&Rep::root`. The tree's root field
       * carries no meta bits, so mask defaults to zero.
       */
      constexpr Handle(uintptr_t* v) : val(v) {}

      [[nodiscard]] uintptr_t get() const
      {
        return *val & ~mask;
      }

      Handle& operator=(uintptr_t v)
      {
        SNMALLOC_ASSERT((v & mask) == 0);
        *val = v | (*val & mask);
        return *this;
      }

      bool operator!=(const Handle& other) const
      {
        return val != other.val;
      }

      uintptr_t printable_address() const
      {
        return reinterpret_cast<uintptr_t>(val);
      }
    };

  private:
    template<size_t UnitIdx>
    static InplaceNode<bounds>* unit_at(uintptr_t addr)
    {
      auto cap = Authmap::amplify_from_address(addr + UnitIdx * UNIT_SIZE);
      return static_cast<InplaceNode<bounds>*>(cap.unsafe_ptr());
    }

    /**
     * Tree rep shared by `BinRep` and `RangeRep`. `UnitIdx` is the
     * block-relative unit (0 or 1) that holds this rep's node;
     * `MetaMask` covers the bits in that unit's `word_one` owned
     * by this rep (red + variant for `BinRep`, red only for
     * `RangeRep`) and is preserved across `set`.
     *
     * Convention (mirrors `PagemapRep`): direction `true` selects
     * `word_one` (the meta-bearing word); direction `false`
     * selects `word_two`.
     */
    template<size_t UnitIdx, uintptr_t MetaMask, const char* Name>
    struct TreeRep
    {
      using Handle = InplaceRep::Handle;
      using Contents = uintptr_t;

      static constexpr Contents null = 0;
      static constexpr Contents root = 0;

      static Handle ref(bool direction, Contents k)
      {
        // Sentinel handle for the null key, mirroring
        // `PagemapRep::TreeRep::ref`. Reads return 0; writes are
        // disallowed by the tree's algorithm but the storage is
        // still backing in case of accidental writes during
        // debugging.
        static uintptr_t null_entry = 0;
        if (SNMALLOC_UNLIKELY(k == 0))
          return Handle{&null_entry, 0};
        auto* node = unit_at<UnitIdx>(k);
        return direction ? Handle{&node->word_one, MetaMask} :
                           Handle{&node->word_two, 0};
      }

      static Contents get(Handle h)
      {
        return h.get();
      }

      static void set(Handle h, Contents v)
      {
        h = v;
      }

      static bool is_red(Contents k)
      {
        if (k == 0)
          return false;
        return (unit_at<UnitIdx>(k)->word_one & RED_BIT) != 0;
      }

      static void set_red(Contents k, bool new_is_red)
      {
        auto* w = &unit_at<UnitIdx>(k)->word_one;
        if (((*w & RED_BIT) != 0) != new_is_red)
          *w ^= RED_BIT;
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

    static constexpr char BIN_REP_NAME[] = "InplaceBinRep";
    static constexpr char RANGE_REP_NAME[] = "InplaceRangeRep";

  public:
    using BinRep = TreeRep<0, BIN_META_MASK, BIN_REP_NAME>;
    using RangeRep = TreeRep<1, RANGE_META_MASK, RANGE_REP_NAME>;

    static ArenaVariant get_variant(uintptr_t addr)
    {
      auto w = unit_at<0>(addr)->word_one;
      return static_cast<ArenaVariant>((w & VARIANT_MASK) >> VARIANT_SHIFT);
    }

    static void set_variant(uintptr_t addr, ArenaVariant v)
    {
      auto* w = &unit_at<0>(addr)->word_one;
      *w = (*w & ~VARIANT_MASK) | (static_cast<uintptr_t>(v) << VARIANT_SHIFT);
    }

    /**
     * Exact byte size for `Large` blocks. Stored as a plain
     * `uintptr_t` in unit 2's `word_one`; unlike `PagemapRep` we
     * do not need to compress (the pagemap word has reserved low
     * bits but our in-band word has the full width).
     */
    static size_t get_large_size(uintptr_t addr)
    {
      return static_cast<size_t>(unit_at<2>(addr)->word_one);
    }

    static void set_large_size(uintptr_t addr, size_t size)
    {
      SNMALLOC_ASSERT((size & (UNIT_SIZE - 1)) == 0);
      unit_at<2>(addr)->word_one = static_cast<uintptr_t>(size);
    }

    /**
     * Refuse consolidation across `MIN_CHUNK_SIZE` boundaries.
     * `SmallArenaRange::add_range_impl` splits incoming ranges at
     * chunk boundaries, but does not eagerly merge across them on
     * the wrapper side; this check is what stops `Arena`
     * from later merging two adjacent intra-chunk fragments that
     * happen to abut the same chunk boundary, which would create a
     * free block straddling chunks. Chunk-aligned `higher_addr`
     * means the lower neighbour ends at a chunk boundary — refuse.
     */
    static bool can_consolidate(uintptr_t higher_addr)
    {
      return (higher_addr & (MIN_CHUNK_SIZE - 1)) != 0;
    }
  };
} // namespace snmalloc
