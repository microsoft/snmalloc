#pragma once

#include "../pal/pal.h"
#include "arena.h"
#include "empty_range.h"
#include "inplacerep.h"
#include "range_helpers.h"

namespace snmalloc
{
  /**
   * Small-grained range backed by `Arena` with in-band
   * (`InplaceRep`) tree-node storage. Serves blocks of any
   * unit-aligned size — not restricted to powers of two — for
   * `SlabMetadata` allocations.
   *
   * Each arena instance covers exactly one chunk
   * (`MAX_SIZE_BITS = MIN_CHUNK_BITS`): refill takes one chunk
   * from the parent, sub-chunk fragments live in the arena,
   * consolidated whole chunks flow back to the parent.
   */
  template<typename Authmap>
  struct SmallArenaRange
  {
    template<typename ParentRange = EmptyRange<>>
    class Type : public ContainsParent<ParentRange>
    {
    public:
      using ChunkBounds = typename ParentRange::ChunkBounds;

    private:
      using ContainsParent<ParentRange>::parent;

      using RepT = InplaceRep<Authmap, ChunkBounds>;
      static constexpr size_t MIN_BITS = RepT::MIN_BITS;

      Arena<RepT, MIN_BITS, MIN_CHUNK_BITS> arena;

    public:
      static constexpr size_t UNIT_SIZE = RepT::UNIT_SIZE;

    private:
      /**
       * Split `[base, base+length)` at chunk boundaries.
       * Intra-chunk fragments are unit-trimmed and submitted to
       * the arena; segments that begin and end chunk-aligned go
       * to the parent. Accepts arbitrary unaligned input —
       * `dealloc_meta_data` forwards `make()`'s unaligned spare
       * here; sub-unit edges are discarded by design.
       */
      void add_range_impl(CapPtr<void, ChunkBounds> base, size_t length)
      {
        uintptr_t lo = base.unsafe_uintptr();
        uintptr_t hi = lo + length;

        while (lo < hi)
        {
          uintptr_t chunk_end = bits::align_up(lo + 1, MIN_CHUNK_SIZE);
          uintptr_t seg_end = bits::min(hi, chunk_end);

          if (
            lo == bits::align_down(lo, MIN_CHUNK_SIZE) && seg_end == chunk_end)
          {
            auto chunk_base = CapPtr<void, ChunkBounds>::unsafe_from(
              reinterpret_cast<void*>(lo));
            parent.dealloc_range(chunk_base, MIN_CHUNK_SIZE);
          }
          else
          {
            uintptr_t f_lo = bits::align_up(lo, UNIT_SIZE);
            uintptr_t f_hi = bits::align_down(seg_end, UNIT_SIZE);
            if (f_lo < f_hi)
            {
              auto [ov_a, ov_s] = arena.add_block(f_lo, f_hi - f_lo);
              if (ov_a != 0)
              {
                // Arena consolidated up to MAX_SIZE_BITS = chunk:
                // hand the whole-chunk piece back to the parent.
                auto ov_base = CapPtr<void, ChunkBounds>::unsafe_from(
                  reinterpret_cast<void*>(ov_a));
                parent.dealloc_range(ov_base, ov_s);
              }
            }
          }

          lo = seg_end;
        }
      }

      CapPtr<void, ChunkBounds> refill(size_t size)
      {
        auto refill_range = parent.alloc_range(MIN_CHUNK_SIZE);
        if (refill_range == nullptr)
          return nullptr;

        add_range_impl(
          pointer_offset(refill_range, size), MIN_CHUNK_SIZE - size);

        return refill_range;
      }

    public:
      static constexpr bool Aligned = true;
      static_assert(ParentRange::Aligned, "ParentRange must be aligned");

      static constexpr bool ConcurrencySafe = false;

      constexpr Type() = default;

      CapPtr<void, ChunkBounds> alloc_range(size_t size)
      {
        SNMALLOC_ASSERT((size & (UNIT_SIZE - 1)) == 0);

        if (size >= MIN_CHUNK_SIZE)
          return parent.alloc_range(size);

        uintptr_t a = arena.remove_block(size);
        if (a != 0)
          return CapPtr<void, ChunkBounds>::unsafe_from(
            reinterpret_cast<void*>(a));

        return refill(size);
      }

      /**
       * Allocate `align`-aligned space large enough for `size`,
       * donating the unit-aligned tail back to the arena.
       *
       * Requests `requested = align_up(size, align)` bytes; because
       * `align` is pow2 and `requested` is a multiple of `align`,
       * `Arena`'s carve returns an `align`-aligned base
       * without a caller-side over-allocate-and-trim. The tail
       * `[align_up(size, UNIT_SIZE), requested)` is donated via
       * `add_range_impl`. The sub-unit slice
       * `[size, align_up(size, UNIT_SIZE))` cannot be represented
       * and is leaked — pre-round `size` to `UNIT_SIZE` to avoid it.
       */
      CapPtr<void, ChunkBounds> alloc_size_with_align(size_t size, size_t align)
      {
        SNMALLOC_ASSERT(size > 0);
        SNMALLOC_ASSERT(bits::is_pow2(align));
        SNMALLOC_ASSERT(align >= UNIT_SIZE);
        SNMALLOC_ASSERT(align <= MIN_CHUNK_SIZE);

        size_t requested = bits::align_up(size, align);
        auto p = alloc_range(requested);
        if (p == nullptr)
          return nullptr;

        size_t used = bits::align_up(size, UNIT_SIZE);
        if (used < requested)
        {
          add_range_impl(pointer_offset(p, used), requested - used);
        }

        return p;
      }

      // No precondition on `size`: sub-unit edges discarded.
      void dealloc_range(CapPtr<void, ChunkBounds> base, size_t size)
      {
        add_range_impl(base, size);
      }
    };
  };
} // namespace snmalloc
