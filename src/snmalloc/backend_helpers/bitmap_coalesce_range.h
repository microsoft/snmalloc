#pragma once

#include "../ds/ds.h"
#include "../mem/mem.h"
#include "bitmap_coalesce.h"
#include "bitmap_coalesce_helpers.h"
#include "empty_range.h"
#include "range_helpers.h"

namespace snmalloc
{
  /**
   * Pagemap accessor for the BitmapCoalesceRange.
   *
   * Implements the Rep concept required by BitmapCoalesce<Rep>.
   * Word::One = next pointer,
   * Word::Two = size (boundary tag).
   */
  template<SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap>
  class BitmapCoalesceRep
  {
    template<typename P, typename = void>
    struct pagemap_has_bounds : stl::false_type
    {};

    template<typename P>
    struct pagemap_has_bounds<P, stl::void_t<decltype(P::get_bounds())>>
    : stl::true_type
    {};

    static bool is_out_of_bounds(address_t addr)
    {
      if constexpr (pagemap_has_bounds<Pagemap>::value)
      {
        auto [pm_base, pm_size] = Pagemap::get_bounds();
        return (addr - pm_base) >= pm_size;
      }
      else
      {
        UNUSED(addr);
        return false;
      }
    }

  public:
    static address_t get_next(address_t addr)
    {
      auto& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(addr));
      return entry.get_backend_word(MetaEntryBase::Word::One).get();
    }

    static void set_next(address_t addr, address_t next)
    {
      auto& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(addr));
      entry.get_backend_word(MetaEntryBase::Word::One) = next;
    }

    static size_t get_size(address_t addr)
    {
      auto& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(addr));
      return static_cast<size_t>(
        entry.get_backend_word(MetaEntryBase::Word::Two).get());
    }

    static void set_size(address_t addr, size_t size)
    {
      auto& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(addr));
      entry.get_backend_word(MetaEntryBase::Word::Two) = size;
    }

    static void set_boundary_tags(address_t addr, size_t size)
    {
      set_size(addr, size);
      if (size > MIN_CHUNK_SIZE)
      {
        set_size(addr + size - MIN_CHUNK_SIZE, size);
      }
    }

    static bool is_free_block(address_t addr)
    {
      if (addr == 0)
        return false;
      if (is_out_of_bounds(addr))
        return false;
      auto& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(addr));
      return entry.is_backend_owned() && entry.is_coalesce_free();
    }

    static bool is_boundary(address_t addr)
    {
      if (is_out_of_bounds(addr))
        return true;
      auto& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(addr));
      return entry.is_boundary();
    }

    static void set_boundary(address_t addr)
    {
      auto& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(addr));
      entry.set_boundary();
    }

    static void set_coalesce_free(address_t addr)
    {
      auto& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(addr));
      entry.set_coalesce_free();
    }

    static void clear_coalesce_free(address_t addr)
    {
      auto& entry =
        Pagemap::template get_metaentry_mut<false>(address_cast(addr));
      entry.clear_coalesce_free();
    }
  };

  /**
   * Pipeline adapter for the bitmap-indexed coalescing allocator.
   *
   * Thin wrapper that connects BitmapCoalesce<Rep> to snmalloc's range
   * pipeline, providing alloc_range / dealloc_range with post-allocation
   * carving and gradual warm-up refill.  Drop-in replacement for
   * LargeBuddyRange.
   *
   * Template parameters:
   *   REFILL_SIZE_BITS     - Maximum refill size from parent (log2).
   *   MAX_SIZE_BITS        - Maximum block size this range manages (log2).
   *   Pagemap              - Pagemap type for boundary tags & linked lists.
   *   MIN_REFILL_SIZE_BITS - Minimum refill size from parent (log2).
   */
  template<
    size_t REFILL_SIZE_BITS,
    size_t MAX_SIZE_BITS,
    SNMALLOC_CONCEPT(IsWritablePagemap) Pagemap,
    size_t MIN_REFILL_SIZE_BITS = 0>
  class BitmapCoalesceRange
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

      using Rep = BitmapCoalesceRep<Pagemap>;
      using BC = BitmapCoalesceHelpers<MAX_SIZE_BITS>;

      BitmapCoalesce<Rep, MAX_SIZE_BITS> bc{};

      size_t requested_total = 0;

      // ----- Refill from parent -----

      capptr::Arena<void> refill(size_t size, size_t alignment)
      {
        if (ParentRange::Aligned)
        {
          // Gradual warm-up heuristic (same as LargeBuddyRange).
          size_t refill_size = bits::min(REFILL_SIZE, requested_total);
          refill_size = bits::max(refill_size, MIN_REFILL_SIZE);
          refill_size = bits::max(refill_size, size);
          refill_size = bits::next_pow2(refill_size);

          auto refill_range = parent.alloc_range(refill_size);
          if (refill_range != nullptr)
          {
            requested_total += refill_size;
            if (refill_size > size)
            {
              Rep::set_boundary(refill_range.unsafe_uintptr() + size);
              bc.add_fresh_range(
                refill_range.unsafe_uintptr() + size, refill_size - size);
            }
            SNMALLOC_ASSERT(
              (refill_range.unsafe_uintptr() % alignment) == 0);
          }
          return refill_range;
        }

        // Unaligned parent: overallocate, carve, return remainders.
        size_t extra = size + alignment;
        if (extra < size) // overflow check
          return nullptr;

        size_t refill_size = bits::min(REFILL_SIZE, requested_total);
        refill_size = bits::max(refill_size, MIN_REFILL_SIZE);
        refill_size = bits::max(refill_size, extra);

        while (extra <= refill_size)
        {
          auto range = parent.alloc_range(refill_size);
          if (range != nullptr)
          {
            requested_total += refill_size;
            auto base = range.unsafe_uintptr();

            auto aligned_base = bits::align_up(base, alignment);
            SNMALLOC_ASSERT(aligned_base + size <= base + refill_size);

            if (aligned_base > base)
            {
              Rep::set_boundary(aligned_base);
            }

            auto right_start = aligned_base + size;
            auto range_end = base + refill_size;
            if (right_start < range_end)
            {
              Rep::set_boundary(right_start);
            }

            if (aligned_base > base)
            {
              bc.add_fresh_range(base, aligned_base - base);
            }

            if (right_start < range_end)
            {
              bc.add_fresh_range(right_start, range_end - right_start);
            }

            SNMALLOC_ASSERT((aligned_base % alignment) == 0);
            return capptr::Arena<void>::unsafe_from(
              reinterpret_cast<void*>(aligned_base));
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
       * Allocate a range of the given size.
       * Size must be >= MIN_CHUNK_SIZE.
       * Returns a naturally-aligned block of at least the requested size.
       */
      NOINLINE capptr::Arena<void> alloc_range(size_t size)
      {
        SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);

        // Bypass for sizes at or above our maximum.
        if (size >= bits::mask_bits(MAX_SIZE_BITS))
        {
          if (ParentRange::Aligned)
            return parent.alloc_range(size);
          return nullptr;
        }

        // Round up to a valid sizeclass for the bitmap search.
        size_t n_chunks = size / MIN_CHUNK_SIZE;
        size_t sc_chunks = BC::round_up_sizeclass(n_chunks);
        if (sc_chunks == 0)
          sc_chunks = 1;
        size_t sc_bytes = sc_chunks * MIN_CHUNK_SIZE;

        // Required alignment: natural alignment for the rounded-up sizeclass.
        size_t sc_align =
          BC::natural_alignment(sc_chunks) * MIN_CHUNK_SIZE;

        auto result = bc.remove_block(sc_bytes);
        if (result.addr != 0)
        {
          SNMALLOC_ASSERT(result.size >= sc_bytes);
          SNMALLOC_ASSERT(result.size % MIN_CHUNK_SIZE == 0);

          // Carve: find aligned address within the block.
          address_t aligned_addr = bits::align_up(result.addr, sc_align);
          SNMALLOC_ASSERT(aligned_addr + size <= result.addr + result.size);

          size_t prefix = aligned_addr - result.addr;
          size_t suffix = result.addr + result.size - aligned_addr - size;

          // Return prefix and suffix remainders to the free pool.
          // No boundary bit needed: this carving happens under the lock,
          // and subsequent frees will correctly coalesce.
          if (prefix > 0)
            bc.add_fresh_range(result.addr, prefix);
          if (suffix > 0)
            bc.add_fresh_range(aligned_addr + size, suffix);

          SNMALLOC_ASSERT((aligned_addr % sc_align) == 0);
          return capptr::Arena<void>::unsafe_from(
            reinterpret_cast<void*>(aligned_addr));
        }

        return refill(size, sc_align);
      }

      /**
       * Deallocate a range of memory.
       * Size must be >= MIN_CHUNK_SIZE.
       */
      NOINLINE void dealloc_range(capptr::Arena<void> base, size_t size)
      {
        SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);
        SNMALLOC_ASSERT(size % MIN_CHUNK_SIZE == 0);
        SNMALLOC_ASSERT(
          (base.unsafe_uintptr() %
           (BC::natural_alignment(size / MIN_CHUNK_SIZE) * MIN_CHUNK_SIZE)) ==
          0);

        if constexpr (MAX_SIZE_BITS != (bits::BITS - 1))
        {
          if (size >= bits::mask_bits(MAX_SIZE_BITS))
          {
            parent.dealloc_range(base, size);
            return;
          }
        }

        bc.add_block(base.unsafe_uintptr(), size);
      }
    };
  };
} // namespace snmalloc
