#pragma once

#include "../pal/pal.h"

/**
 * This file contains all the code for transforming transforming sizes to
 * sizeclasses and back.  It also contains various sizeclass pre-calculated
 * tables for operations based on size class such as `modulus` and `divisible
 * by`, and constants for the slab based allocator.
 *
 * TODO:  Due to the current structure for constexpr evaluation this file does
 * not well delimit internal versus external APIs. Some refactoring should be
 * done.
 */

namespace snmalloc
{
  using chunksizeclass_t = size_t;

  // Capped to `bits::BITS - 1` so `MAX_LARGE_SIZECLASS_SIZE` fits in
  // `size_t` on 32-bit platforms where `DefaultPal::address_bits ==
  // bits::BITS`.
  constexpr size_t ENCODED_ADDRESS_BITS =
    bits::min(DefaultPal::address_bits, bits::BITS - 1);

  // Large classes follow on directly from small classes in the global
  // exp+mantissa scheme: `(ENCODED_ADDRESS_BITS - MAX_SMALL_SIZECLASS_BITS)`
  // mantissa cycles of `2^INTERMEDIATE_BITS` entries each.
  constexpr size_t NUM_LARGE_CLASSES =
    (ENCODED_ADDRESS_BITS - MAX_SMALL_SIZECLASS_BITS) << INTERMEDIATE_BITS;

  // Slot 0 of the table is reserved as the unmapped sentinel, hence +1.
  constexpr size_t SIZECLASS_BITS =
    bits::next_pow2_bits_const(1 + NUM_SMALL_SIZECLASSES + NUM_LARGE_CLASSES);

  constexpr size_t SIZECLASS_REP_SIZE = bits::one_at_bit(SIZECLASS_BITS);

  // Width of the per-chunk slab-offset field packed immediately above the
  // sizeclass in `ras`. The worst-case slab count for any non-pow2 large
  // class with `INTERMEDIATE_BITS = M` is `2^(M+1)`; `M + 1` bits cover
  // the maximum index. `compute_max_large_slab_index` static_asserts the
  // bound against the actual table below.
  constexpr size_t OFFSET_BITS = INTERMEDIATE_BITS + 1;

  // `ras & COMBINED_MASK` directly indexes the `(sizeclass, offset)` table
  // row, which already carries `offset_bytes = offset * slab_size`.
  constexpr size_t COMBINED_BITS = SIZECLASS_BITS + OFFSET_BITS;
  constexpr size_t COMBINED_REP_SIZE = bits::one_at_bit(COMBINED_BITS);

  // Largest size representable by the uniform sizeclass encoding;
  // requests larger than this must be failed before
  // `size_to_sizeclass_full`.
  constexpr size_t MAX_LARGE_SIZECLASS_SIZE =
    bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(
      NUM_SMALL_SIZECLASSES + NUM_LARGE_CLASSES - 1);

  static_assert(
    MAX_LARGE_SIZECLASS_SIZE == bits::one_at_bit(ENCODED_ADDRESS_BITS),
    "MAX_LARGE_SIZECLASS_SIZE must equal 2 ^ ENCODED_ADDRESS_BITS; if this "
    "fails, the exp+mantissa math does not match NUM_LARGE_CLASSES.");
  static_assert(
    ENCODED_ADDRESS_BITS > MAX_SMALL_SIZECLASS_BITS,
    "ENCODED_ADDRESS_BITS must exceed MAX_SMALL_SIZECLASS_BITS so the large "
    "range is non-empty.");

  /**
   * Sizeclass identifier shared by small and large allocations:
   *
   *   value == 0                              : sentinel (unmapped)
   *   value ∈ [1, 1 + NUM_SMALL_SIZECLASSES)  : small, sc = value - 1
   *   value ∈ [1 + NUM_SMALL_SIZECLASSES, ...): large
   *
   * Indexes `sizeclass_metadata` directly; slot 0 is zero-padded so the
   * sentinel flows through fast-path lookups without a branch.
   */
  class sizeclass_t
  {
    size_t value{0};

    constexpr sizeclass_t(size_t value) : value(value) {}

  public:
    constexpr sizeclass_t() = default;

    static constexpr sizeclass_t from_small_class(smallsizeclass_t sc)
    {
      SNMALLOC_ASSERT(sc < NUM_SMALL_SIZECLASSES);
      return {sc + 1};
    }

    /**
     * Construct from a large class index `lc` in [0, NUM_LARGE_CLASSES).
     * Large classes are stored as a contiguous run immediately after the
     * small range and the sentinel slot.
     */
    static constexpr sizeclass_t from_large_class(size_t large_class)
    {
      SNMALLOC_ASSERT(large_class < NUM_LARGE_CLASSES);
      return {1 + NUM_SMALL_SIZECLASSES + large_class};
    }

    static constexpr sizeclass_t from_raw(size_t raw)
    {
      return {raw};
    }

    constexpr smallsizeclass_t as_small()
    {
      SNMALLOC_ASSERT(is_small());
      return smallsizeclass_t(value - 1);
    }

    constexpr chunksizeclass_t as_large()
    {
      SNMALLOC_ASSERT(!is_small() && !is_default());
      return value - 1 - NUM_SMALL_SIZECLASSES;
    }

    constexpr size_t raw()
    {
      return value;
    }

    constexpr bool is_small()
    {
      // Sentinel (value == 0) underflows past NUM_SMALL_SIZECLASSES.
      return (value - 1) < NUM_SMALL_SIZECLASSES;
    }

    constexpr bool is_default()
    {
      return value == 0;
    }

    constexpr bool operator==(sizeclass_t other)
    {
      return value == other.value;
    }

    constexpr bool operator!=(sizeclass_t other)
    {
      return value != other.value;
    }
  };

  /**
   * (sizeclass, per-chunk slab offset) packed into the low `COMBINED_BITS`
   * of a pagemap entry's `remote_and_sizeclass`. Non-zero offsets occur
   * only for interior chunks of non-pow2 large allocations; the offset
   * lets `start_of_object` recover the allocation base.
   *
   * Distinct from `sizeclass_t` so `is_small()` / `as_small()` /
   * `as_large()` cannot be called on a value carrying offset bits, and so
   * the offset can never be synthesised: constructing a value requires
   * supplying both components explicitly, or going through `from_raw`
   * with bits read from storage.
   */
  class offset_and_sizeclass_t
  {
    size_t value{0};

    constexpr offset_and_sizeclass_t(size_t value) : value(value) {}

  public:
    constexpr offset_and_sizeclass_t() = default;

    constexpr offset_and_sizeclass_t(sizeclass_t sc, size_t offset)
    : value(sc.raw() | (offset << SIZECLASS_BITS))
    {
      SNMALLOC_ASSERT(offset < (size_t{1} << OFFSET_BITS));
    }

    static constexpr offset_and_sizeclass_t from_raw(size_t raw)
    {
      return {raw};
    }

    constexpr size_t raw() const
    {
      return value;
    }

    constexpr sizeclass_t sizeclass() const
    {
      return sizeclass_t::from_raw(value & (SIZECLASS_REP_SIZE - 1));
    }

    constexpr size_t offset() const
    {
      return (value >> SIZECLASS_BITS) & ((size_t{1} << OFFSET_BITS) - 1);
    }

    constexpr bool operator==(offset_and_sizeclass_t other) const
    {
      return value == other.value;
    }
  };

  using sizeclass_compress_t = uint8_t;

  /**
   * Per-`offset_and_sizeclass_t` metadata for `start_of_object` —
   * recovering the allocation base from an interior pointer.
   *
   * Sized to a power of two (4 × `size_t` = 32 bytes) so the table
   * stride collapses to a single shift in the
   * `__malloc_start_pointer` hot path.
   */
  struct sizeclass_data_start
  {
    size_t size;
    // We store the mask as it is used more on the fast path, and the size of
    // the slab.
    size_t slab_mask;
    // Table of constants for reciprocal division for each sizeclass.
    size_t div_mult;
    // `offset * slab_size`, precomputed. Zero for `offset == 0` rows.
    size_t offset_bytes;
  };

  static_assert(
    sizeof(sizeclass_data_start) == 4 * sizeof(size_t),
    "sizeclass_data_start must be a power-of-two stride for single-shift "
    "indexing in start_of_object");

  /**
   * Per-`sizeclass_t` metadata for `is_start_of_object` — the
   * Lemire-style alignment check used by check-build dealloc and
   * debug asserts.
   *
   * `slab_mask` is duplicated here (also held in `sizeclass_data_start`)
   * so the alignment check loads from a single row instead of straddling
   * two tables.
   */
  struct sizeclass_data_align
  {
    size_t slab_mask;
    size_t mod_zero_mult;
  };

  /**
   * Per-`sizeclass_t` thresholds used when initialising a slab —
   * cold-path data consumed at slab allocation/refill time.
   */
  struct sizeclass_data_slab
  {
    uint16_t capacity;
    uint16_t waking;
  };

  static_assert(sizeof(sizeclass_data_slab::capacity) * 8 > MAX_CAPACITY_BITS);

  struct SizeClassTable
  {
    // `start_` is indexed by an `offset_and_sizeclass_t` (Word::Two of
    // the pagemap entry & COMBINED_MASK). The first SIZECLASS_REP_SIZE
    // rows have offset == 0; subsequent rows carry the offset_bytes
    // needed for `start_of_object` on non-pow2 large interior chunks.
    ModArray<COMBINED_REP_SIZE, sizeclass_data_start> start_{};
    ModArray<SIZECLASS_REP_SIZE, sizeclass_data_align> align_{};
    ModArray<SIZECLASS_REP_SIZE, sizeclass_data_slab> slab_{};

    size_t DIV_MULT_SHIFT{0};

    [[nodiscard]] constexpr sizeclass_data_start& start(sizeclass_t index)
    {
      return start_[index.raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_start start(sizeclass_t index) const
    {
      return start_[index.raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_start&
    start(offset_and_sizeclass_t osc)
    {
      return start_[osc.raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_start
    start(offset_and_sizeclass_t osc) const
    {
      return start_[osc.raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_start&
    start_small(smallsizeclass_t sc)
    {
      return start_[sizeclass_t::from_small_class(sc).raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_start
    start_small(smallsizeclass_t sc) const
    {
      return start_[sizeclass_t::from_small_class(sc).raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_align& align(sizeclass_t index)
    {
      return align_[index.raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_align align(sizeclass_t index) const
    {
      return align_[index.raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_slab& slab(sizeclass_t index)
    {
      return slab_[index.raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_slab slab(sizeclass_t index) const
    {
      return slab_[index.raw()];
    }

    constexpr SizeClassTable()
    {
      // Sentinel slot (sizeclass_t{} / raw 0) covers any address whose
      // pagemap entry is unmapped or owned by the backend — including
      // foreign (non-snmalloc) heap addresses reached via the
      // bounds-checked memcpy shim before snmalloc has seen them.
      // `slab_mask = ~size_t(0)` makes `start_of_object` collapse
      // `addr & ~slab_mask` to 0 and `index_in_object` to `addr`, so
      // `remaining_bytes = sentinel.size - addr` underflows to a very
      // large value and any memcpy bound check trivially passes the
      // sentinel through to the destination's native checks.
      start_[0].slab_mask = ~size_t(0);

      size_t max_capacity = 0;

      for (smallsizeclass_t sizeclass(0); sizeclass < NUM_SMALL_SIZECLASSES;
           sizeclass++)
      {
        auto& meta = start_small(sizeclass);
        auto sc = sizeclass_t::from_small_class(sizeclass);

        size_t rsize =
          bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(
            sizeclass);
        meta.size = rsize;
        size_t slab_bits = bits::max(
          bits::next_pow2_bits_const(MIN_OBJECT_COUNT * rsize), MIN_CHUNK_BITS);

        meta.slab_mask = bits::mask_bits(slab_bits);
        align(sc).slab_mask = meta.slab_mask;

        auto& meta_slab = slab(sc);
        meta_slab.capacity =
          static_cast<uint16_t>((meta.slab_mask + 1) / rsize);

        meta_slab.waking = mitigations(random_larger_thresholds) ?
          static_cast<uint16_t>(meta_slab.capacity / 4) :
          static_cast<uint16_t>(bits::min((meta_slab.capacity / 4), 32));

        if (meta_slab.capacity > max_capacity)
        {
          max_capacity = meta_slab.capacity;
        }
      }

      // Get maximum precision to calculate largest division range.
      DIV_MULT_SHIFT = bits::BITS - bits::next_pow2_bits_const(max_capacity);

      for (smallsizeclass_t sizeclass(0); sizeclass < NUM_SMALL_SIZECLASSES;
           sizeclass++)
      {
        // Calculate reciprocal division constant.
        auto& meta = start_small(sizeclass);
        meta.div_mult = (bits::mask_bits(DIV_MULT_SHIFT) / meta.size) + 1;

        size_t zero = 0;
        align(sizeclass_t::from_small_class(sizeclass)).mod_zero_mult =
          (~zero / meta.size) + 1;
      }

      for (size_t lc = 0; lc < NUM_LARGE_CLASSES; lc++)
      {
        auto lsc = sizeclass_t::from_large_class(lc);
        auto& meta = start(lsc);
        size_t size =
          bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(
            NUM_SMALL_SIZECLASSES + lc);
        meta.size = size;
        // `slab_mask = (natural alignment of size) - 1`; for pow2 sizes
        // this equals size - 1, for non-pow2 mantissa steps it is the
        // slab granularity at which the allocation tiles.
        size_t align_bytes = size & (~size + 1);
        meta.slab_mask = align_bytes - 1;
        align(lsc).slab_mask = meta.slab_mask;
        // slab_mask handles the math; identity values neutralise the
        // mod/div reciprocals.
        align(lsc).mod_zero_mult = 1;
        meta.div_mult = 0;
      }

      // Populate offset > 0 rows: same as the (sc, 0) row but with
      // `offset_bytes = offset * slab_size` so that `start_of_object`
      // collapses to `(addr & ~slab_mask) - offset_bytes`. Read when
      // the backend writes per-chunk offsets for multi-slab-tile
      // reservations.
      for (size_t sc_raw = 0; sc_raw < SIZECLASS_REP_SIZE; sc_raw++)
      {
        const auto& base = start_[sc_raw];
        const size_t slab_size = base.slab_mask + 1;
        for (size_t offset = 1; offset < (size_t{1} << OFFSET_BITS); offset++)
        {
          auto& row = start_[sc_raw | (offset << SIZECLASS_BITS)];
          row.size = base.size;
          row.slab_mask = base.slab_mask;
          row.div_mult = base.div_mult;
          row.offset_bytes = offset * slab_size;
        }
      }
    }
  };

  constexpr SizeClassTable sizeclass_metadata = SizeClassTable();

  // Sentinel must remain zero-initialised so fast-path lookups via
  // `start(sc)` return zero size without a branch. Slab_mask is
  // `~size_t(0)` so foreign-pointer `remaining_bytes` underflows to a
  // huge value (see `SizeClassTable::SizeClassTable`).
  static_assert(
    sizeclass_metadata.start(sizeclass_t{}).size == 0,
    "sentinel slot must have size 0");
  static_assert(
    sizeclass_metadata.start(sizeclass_t{}).slab_mask == ~size_t(0),
    "sentinel slot must have slab_mask ~0 for foreign-pointer "
    "remaining_bytes underflow");

  static_assert(
    bits::BITS - sizeclass_metadata.DIV_MULT_SHIFT <= MAX_CAPACITY_BITS);

  // Largest slab index for any large class: `OFFSET_BITS` must cover
  // it. Each large allocation reserves exactly `meta.size` bytes (a
  // positive multiple of `slab_size`), so the largest `slab_index`
  // the pagemap loop in `Backend::alloc_chunk` writes is
  // `meta.size / slab_size - 1`.
  constexpr size_t compute_max_large_slab_index()
  {
    size_t max_idx = 0;
    for (size_t lc = 0; lc < NUM_LARGE_CLASSES; lc++)
    {
      const auto& meta =
        sizeclass_metadata.start(sizeclass_t::from_large_class(lc));
      const size_t slab_size = meta.slab_mask + 1;
      const size_t idx = (meta.size / slab_size) - 1;
      if (idx > max_idx)
        max_idx = idx;
    }
    return max_idx;
  }

  static_assert(
    compute_max_large_slab_index() < (size_t{1} << OFFSET_BITS),
    "OFFSET_BITS must cover the worst-case slab index for any large class");

  constexpr size_t DIV_MULT_SHIFT = sizeclass_metadata.DIV_MULT_SHIFT;

  constexpr size_t sizeclass_to_size(smallsizeclass_t sizeclass)
  {
    return sizeclass_metadata.start_small(sizeclass).size;
  }

  constexpr size_t sizeclass_full_to_size(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.start(sizeclass).size;
  }

  constexpr size_t sizeclass_full_to_slab_size(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.start(sizeclass).slab_mask + 1;
  }

  constexpr size_t sizeclass_to_slab_size(smallsizeclass_t sizeclass)
  {
    return sizeclass_metadata.start_small(sizeclass).slab_mask + 1;
  }

  /**
   * Only wake slab if we have this many free allocations
   *
   * This helps remove bouncing around empty to non-empty cases.
   *
   * It also increases entropy, when we have randomisation.
   */
  constexpr uint16_t threshold_for_waking_slab(smallsizeclass_t sizeclass)
  {
    return sizeclass_metadata.slab(sizeclass_t::from_small_class(sizeclass))
      .waking;
  }

  inline static size_t sizeclass_to_slab_sizeclass(smallsizeclass_t sizeclass)
  {
    size_t ssize = sizeclass_to_slab_size(sizeclass);

    return bits::next_pow2_bits(ssize) - MIN_CHUNK_BITS;
  }

  constexpr size_t slab_sizeclass_to_size(chunksizeclass_t sizeclass)
  {
    return bits::one_at_bit(MIN_CHUNK_BITS + sizeclass);
  }

  constexpr uint16_t sizeclass_to_slab_object_count(smallsizeclass_t sizeclass)
  {
    return sizeclass_metadata.slab(sizeclass_t::from_small_class(sizeclass))
      .capacity;
  }

  SNMALLOC_FAST_PATH constexpr size_t
  slab_index(offset_and_sizeclass_t osc, address_t addr)
  {
    auto meta = sizeclass_metadata.start(osc);
    size_t offset = addr & meta.slab_mask;
    if constexpr (sizeof(offset) >= 8)
    {
      // Only works for 64 bit multiplication, as the following will overflow in
      // 32bit.
      // Based on
      //   https://lemire.me/blog/2019/02/20/more-fun-with-fast-remainders-when-the-divisor-is-a-constant/
      // We are using an adaptation of the "indirect" method.  By using the
      // indirect method we can handle the large power of two classes just with
      // the slab_mask by making the `div_mult` zero. The link uses 128 bit
      // multiplication, we have shrunk the range of the calculation to remove
      // this dependency.
      size_t index = ((offset * meta.div_mult) >> DIV_MULT_SHIFT);
      return index;
    }
    else
    {
      size_t size = meta.size;
      if (size == 0)
        return 0;
      return offset / size;
    }
  }

  /**
   * Recover the start address of the allocation containing `addr`.
   *
   * Branch on `osc.offset() == 0` (testable from bits already loaded
   * into `osc.raw()`, before any metadata-table access). The common
   * case skips the `offset_bytes` field load and four extra arithmetic
   * insns; the slow arm handles non-pow2 large interior chunks where
   * the slab base must be shifted back to the allocation base.
   */
  SNMALLOC_FAST_PATH constexpr address_t
  start_of_object(offset_and_sizeclass_t osc, address_t addr)
  {
    auto meta = sizeclass_metadata.start(osc);
    if (SNMALLOC_LIKELY(osc.offset() == 0))
    {
      address_t slab_base = addr & ~meta.slab_mask;
      size_t index = slab_index(osc, addr);
      return slab_base + (index * meta.size);
    }
    address_t alloc_start = (addr & ~meta.slab_mask) - meta.offset_bytes;
    size_t index = slab_index(osc, addr - alloc_start);
    return alloc_start + (index * meta.size);
  }

  SNMALLOC_FAST_PATH constexpr size_t
  index_in_object(offset_and_sizeclass_t osc, address_t addr)
  {
    return addr - start_of_object(osc, addr);
  }

  SNMALLOC_FAST_PATH constexpr size_t
  remaining_bytes(offset_and_sizeclass_t osc, address_t addr)
  {
    return sizeclass_metadata.start(osc).size - index_in_object(osc, addr);
  }

  /**
   * True iff `addr` is correctly aligned for an object of this
   * sizeclass within its slab. Does NOT check whether `addr` lies in
   * the first slab tile of a non-pow2 large allocation; callers that
   * could be looking at an interior chunk must read the
   * `offset_and_sizeclass_t` from the pagemap and use that overload
   * instead.
   */
  constexpr bool is_start_of_object(sizeclass_t sc, address_t addr)
  {
    auto meta = sizeclass_metadata.align(sc);
    size_t offset = addr & meta.slab_mask;
    // Only works up to certain offsets, exhaustively tested by rounding.cc
    if constexpr (sizeof(offset) >= 8)
    {
      // Only works for 64 bit multiplication, as the following will overflow in
      // 32bit.
      // This is based on:
      //  https://lemire.me/blog/2019/02/20/more-fun-with-fast-remainders-when-the-divisor-is-a-constant/
      return (offset * meta.mod_zero_mult) < meta.mod_zero_mult;
    }
    else
      // Use 32-bit division as considerably faster than 64-bit, and
      // everything fits into 32bits here.
      return static_cast<uint32_t>(offset % sizeclass_full_to_size(sc)) == 0;
  }

  /**
   * True iff `addr` is the start of an object. Interior chunks of
   * non-pow2 large allocations carry `offset_bytes != 0`; only the
   * first slab tile holds an allocation base, so a non-zero
   * `offset_bytes` short-circuits to false.
   */
  constexpr bool is_start_of_object(offset_and_sizeclass_t osc, address_t addr)
  {
    if (sizeclass_metadata.start(osc).offset_bytes != 0)
      return false;
    return is_start_of_object(osc.sizeclass(), addr);
  }

  constexpr SNMALLOC_PURE size_t sizeclass_lookup_index(const size_t s)
  {
    // We subtract and shift to reduce the size of the table, i.e. we don't have
    // to store a value for every size.
    return (s - 1) >> MIN_ALLOC_STEP_BITS;
  }

  constexpr size_t sizeclass_lookup_size =
    sizeclass_lookup_index(MAX_SMALL_SIZECLASS_SIZE) + 1;

  /**
   * This struct is used to statically initialise a table for looking up
   * the correct sizeclass.
   */
  struct SizeClassLookup
  {
    sizeclass_compress_t table[sizeclass_lookup_size] = {{}};

    constexpr SizeClassLookup()
    {
      constexpr sizeclass_compress_t minimum_class =
        static_cast<sizeclass_compress_t>(
          size_to_sizeclass_const(MIN_ALLOC_SIZE));

      /* Some unused sizeclasses is OK, but keep it within reason! */
      static_assert(minimum_class < sizeclass_lookup_size);

      size_t curr = 1;

      sizeclass_compress_t sizeclass = 0;
      for (; sizeclass < minimum_class; sizeclass++)
      {
        for (; curr <=
             sizeclass_metadata.start_small(smallsizeclass_t(sizeclass)).size;
             curr += MIN_ALLOC_STEP_SIZE)
        {
          table[sizeclass_lookup_index(curr)] = minimum_class;
        }
      }

      for (; sizeclass < NUM_SMALL_SIZECLASSES; sizeclass++)
      {
        for (; curr <=
             sizeclass_metadata.start_small(smallsizeclass_t(sizeclass)).size;
             curr += MIN_ALLOC_STEP_SIZE)
        {
          auto i = sizeclass_lookup_index(curr);
          if (i == sizeclass_lookup_size)
            break;
          table[i] = sizeclass;
        }
      }
    }
  };

  constexpr SizeClassLookup sizeclass_lookup = SizeClassLookup();

  constexpr smallsizeclass_t size_to_sizeclass(size_t size)
  {
    if (SNMALLOC_LIKELY(is_small_sizeclass(size)))
    {
      auto index = sizeclass_lookup_index(size);
      SNMALLOC_ASSERT(index < sizeclass_lookup_size);
      return smallsizeclass_t(sizeclass_lookup.table[index]);
    }

    // Check this is not called on large sizes.
    SNMALLOC_ASSERT(size == 0);
    // Map size == 0 to the first sizeclass.
    return smallsizeclass_t(0);
  }

  /**
   * Map a requested size to its sizeclass.
   *
   * Small requests use the dense lookup table. Large requests are
   * encoded with `to_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>`,
   * whose ceil semantic (`v = v - 1; ...`) selects the smallest
   * sizeclass whose size is `>= size`. The raw `size` is passed in
   * directly — the encoding does the rounding.
   */
  static inline sizeclass_t size_to_sizeclass_full(size_t size)
  {
    if (is_small_sizeclass(size))
    {
      return sizeclass_t::from_small_class(size_to_sizeclass(size));
    }
    SNMALLOC_ASSERT(size != 0);
    SNMALLOC_ASSERT(size <= MAX_LARGE_SIZECLASS_SIZE);
    size_t global =
      bits::to_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(size);
    return sizeclass_t::from_large_class(global - NUM_SMALL_SIZECLASSES);
  }

  inline SNMALLOC_FAST_PATH static size_t round_size(size_t size)
  {
    if (is_small_sizeclass(size))
    {
      return sizeclass_to_size(size_to_sizeclass(size));
    }

    if (size == 0)
    {
      // If realloc(ptr, 0) returns nullptr, some consumers treat this as a
      // reallocation failure and abort.  To avoid this, we round up the size of
      // requested allocations to the smallest size class.  This can be changed
      // on any platform that's happy to return nullptr from realloc(ptr,0) and
      // should eventually become a configuration option.
      return sizeclass_to_size(size_to_sizeclass(1));
    }

    if (size > MAX_LARGE_SIZECLASS_SIZE)
    {
      // This size is too large, no rounding should occur as will result in a
      // failed allocation later.
      return size;
    }
    // Large branch: round to the smallest enclosing exp+mantissa
    // sizeclass. Must agree with `round_size`'s small-class branch in
    // semantics: every request rounds to the smallest enclosing
    // class. `DefaultConts::success` (corealloc.h) uses `round_size`
    // to compute the `calloc` zeroing range, so any drift between
    // the actual reservation and `round_size` would over- or
    // under-zero.
    return sizeclass_full_to_size(size_to_sizeclass_full(size));
  }

  /// Returns the alignment that this size naturally has, that is
  /// all allocations of size `size` will be aligned to the returned value.
  inline SNMALLOC_FAST_PATH static size_t natural_alignment(size_t size)
  {
    auto rsize = round_size(size);
    if (size == 0)
      return 1;
    return bits::one_at_bit(bits::ctz(rsize));
  }
} // namespace snmalloc
