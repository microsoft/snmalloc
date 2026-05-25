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

  // Cap the address bits the encoding tries to represent so that
  // `MAX_LARGE_SIZECLASS_SIZE` (= 2 ^ ENCODED_ADDRESS_BITS) always fits in
  // `size_t`. On 64-bit platforms `DefaultPal::address_bits` is already 48,
  // but on 32-bit platforms it equals `bits::BITS` and would otherwise
  // overflow the encoded maximum to 0.
  constexpr size_t ENCODED_ADDRESS_BITS =
    bits::min(DefaultPal::address_bits, bits::BITS - 1);

  // Number of large sizeclasses. Large classes follow on directly from small
  // classes in the global exp+mantissa scheme used by
  // `bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>`. The total
  // span of representable sizes is from MIN_ALLOC_SIZE up to and including
  // 2^ENCODED_ADDRESS_BITS, so the count of large entries beyond the small
  // range is (ENCODED_ADDRESS_BITS - MAX_SMALL_SIZECLASS_BITS) mantissa
  // cycles, each with 2^INTERMEDIATE_BITS entries.
  constexpr size_t NUM_LARGE_CLASSES =
    (ENCODED_ADDRESS_BITS - MAX_SMALL_SIZECLASS_BITS) << INTERMEDIATE_BITS;

  // Bits required to encode any sizeclass value. Slot 0 is reserved as the
  // unmapped/default sentinel, so the count includes a leading +1.
  constexpr size_t SIZECLASS_BITS =
    bits::next_pow2_bits_const(1 + NUM_SMALL_SIZECLASSES + NUM_LARGE_CLASSES);

  // Size of the sizeclass-keyed lookup tables and the alignment that the
  // REMOTE_BACKEND_MARKER constraint requires of RemoteAllocator. There is no
  // separate tag bit: all valid sizeclass raw values are in
  // [0, 1 + NUM_SMALL_SIZECLASSES + NUM_LARGE_CLASSES) and live in the low
  // SIZECLASS_BITS bits of a pagemap word.
  constexpr size_t SIZECLASS_REP_SIZE = bits::one_at_bit(SIZECLASS_BITS);

  // Largest allocation size representable by the uniform sizeclass encoding.
  // Equals `from_exp_mant(NUM_SMALL_SIZECLASSES + NUM_LARGE_CLASSES - 1)`,
  // which for the default config is `2 ^ ENCODED_ADDRESS_BITS`. Requests
  // strictly larger than this cannot be encoded and must be failed before
  // any call to `size_to_sizeclass_full`.
  constexpr size_t MAX_LARGE_SIZECLASS_SIZE =
    bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(
      NUM_SMALL_SIZECLASSES + NUM_LARGE_CLASSES - 1);

  static_assert(
    MAX_LARGE_SIZECLASS_SIZE == bits::one_at_bit(ENCODED_ADDRESS_BITS),
    "MAX_LARGE_SIZECLASS_SIZE must equal 2 ^ ENCODED_ADDRESS_BITS; if this "
    "fails, the exp+mantissa math no longer matches NUM_LARGE_CLASSES.");
  static_assert(
    ENCODED_ADDRESS_BITS > MAX_SMALL_SIZECLASS_BITS,
    "ENCODED_ADDRESS_BITS must exceed MAX_SMALL_SIZECLASS_BITS so the large "
    "range is non-empty.");

  /**
   * Represents a sizeclass identifier shared by small and large allocations
   * using a single uniform encoding:
   *
   *   value == 0                              : unmapped / default sentinel
   *   value ∈ [1, 1 + NUM_SMALL_SIZECLASSES)  : small sizeclass sc = value - 1
   *   value ∈ [1 + NUM_SMALL_SIZECLASSES,
   *           1 + NUM_SMALL_SIZECLASSES + NUM_LARGE_CLASSES)
   *                                           : large class lc =
   *                                             value - 1 -
   * NUM_SMALL_SIZECLASSES
   *
   * Used directly as an index into `sizeclass_metadata`. Slot 0 of that table
   * is zero-padded so the sentinel can flow through the fast-path table
   * lookups without a subtract on the hot path.
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
      // Sentinel (value == 0) underflows to a large positive value, which
      // also fails the comparison — the sentinel is therefore not small.
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

  using sizeclass_compress_t = uint8_t;

  /**
   * This structure contains the fields required for fast paths for sizeclasses.
   */
  struct sizeclass_data_fast
  {
    size_t size;
    // We store the mask as it is used more on the fast path, and the size of
    // the slab.
    size_t slab_mask;
    // Table of constants for reciprocal division for each sizeclass.
    size_t div_mult;
    // Table of constants for reciprocal modulus for each sizeclass.
    size_t mod_zero_mult;
  };

  /**
   * This structure contains the remaining fields required for slow paths for
   * sizeclasses.
   */
  struct sizeclass_data_slow
  {
    uint16_t capacity;
    uint16_t waking;
  };

  static_assert(sizeof(sizeclass_data_slow::capacity) * 8 > MAX_CAPACITY_BITS);

  struct SizeClassTable
  {
    ModArray<SIZECLASS_REP_SIZE, sizeclass_data_fast> fast_{};
    ModArray<SIZECLASS_REP_SIZE, sizeclass_data_slow> slow_{};

    size_t DIV_MULT_SHIFT{0};

    [[nodiscard]] constexpr sizeclass_data_fast& fast(sizeclass_t index)
    {
      return fast_[index.raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_fast fast(sizeclass_t index) const
    {
      return fast_[index.raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_fast& fast_small(smallsizeclass_t sc)
    {
      return fast_[sizeclass_t::from_small_class(sc).raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_fast
    fast_small(smallsizeclass_t sc) const
    {
      return fast_[sizeclass_t::from_small_class(sc).raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_slow& slow(sizeclass_t index)
    {
      return slow_[index.raw()];
    }

    [[nodiscard]] constexpr sizeclass_data_slow slow(sizeclass_t index) const
    {
      return slow_[index.raw()];
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
        auto& meta = fast_small(sizeclass);

        size_t rsize =
          bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(
            sizeclass);
        meta.size = rsize;
        size_t slab_bits = bits::max(
          bits::next_pow2_bits_const(MIN_OBJECT_COUNT * rsize), MIN_CHUNK_BITS);

        meta.slab_mask = bits::mask_bits(slab_bits);

        auto& meta_slow = slow(sizeclass_t::from_small_class(sizeclass));
        meta_slow.capacity =
          static_cast<uint16_t>((meta.slab_mask + 1) / rsize);

        meta_slow.waking = mitigations(random_larger_thresholds) ?
          static_cast<uint16_t>(meta_slow.capacity / 4) :
          static_cast<uint16_t>(bits::min((meta_slow.capacity / 4), 32));

        if (meta_slow.capacity > max_capacity)
        {
          max_capacity = meta_slow.capacity;
        }
      }

      // Get maximum precision to calculate largest division range.
      DIV_MULT_SHIFT = bits::BITS - bits::next_pow2_bits_const(max_capacity);

      for (smallsizeclass_t sizeclass(0); sizeclass < NUM_SMALL_SIZECLASSES;
           sizeclass++)
      {
        // Calculate reciprocal division constant.
        auto& meta = fast_small(sizeclass);
        meta.div_mult = (bits::mask_bits(DIV_MULT_SHIFT) / meta.size) + 1;

        size_t zero = 0;
        meta.mod_zero_mult = (~zero / meta.size) + 1;
      }

      for (size_t lc = 0; lc < NUM_LARGE_CLASSES; lc++)
      {
        auto lsc = sizeclass_t::from_large_class(lc);
        auto& meta = fast(lsc);
        // Continuous global exp+mantissa scheme: small classes occupy
        // global indices [0, NUM_SMALL_SIZECLASSES); large classes occupy
        // [NUM_SMALL_SIZECLASSES, NUM_SMALL_SIZECLASSES + NUM_LARGE_CLASSES).
        size_t size =
          bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(
            NUM_SMALL_SIZECLASSES + lc);
        meta.size = size;
        // Natural alignment of the size: the largest power of two that
        // divides `size`. For pow2 sizes, this equals `size`; for non-pow2
        // mantissa steps it is the slab granularity at which the allocation
        // tiles. `slab_mask = align - 1`.
        size_t align = size & (~size + 1);
        meta.slab_mask = align - 1;
        // The slab_mask will do all the necessary work, so
        // perform identity multiplication for the test.
        meta.mod_zero_mult = 1;
        // The slab_mask will do all the necessary work for division
        // so collapse the calculated offset.
        meta.div_mult = 0;
      }
    }
  };

  constexpr SizeClassTable sizeclass_metadata = SizeClassTable();

  // Slot 0 of `sizeclass_metadata` is the unmapped sentinel; it must remain
  // zero-initialised so fast-path lookups via `fast(sc)` return zero size
  // and slab_mask without needing a sentinel check before indexing.
  static_assert(
    sizeclass_metadata.fast(sizeclass_t{}).size == 0,
    "sentinel slot must have size 0");
  static_assert(
    sizeclass_metadata.fast(sizeclass_t{}).slab_mask == 0,
    "sentinel slot must have slab_mask 0");

  static_assert(
    bits::BITS - sizeclass_metadata.DIV_MULT_SHIFT <= MAX_CAPACITY_BITS);

  constexpr size_t DIV_MULT_SHIFT = sizeclass_metadata.DIV_MULT_SHIFT;

  constexpr size_t sizeclass_to_size(smallsizeclass_t sizeclass)
  {
    return sizeclass_metadata.fast_small(sizeclass).size;
  }

  constexpr size_t sizeclass_full_to_size(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.fast(sizeclass).size;
  }

  constexpr size_t sizeclass_full_to_slab_size(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.fast(sizeclass).slab_mask + 1;
  }

  constexpr size_t sizeclass_to_slab_size(smallsizeclass_t sizeclass)
  {
    return sizeclass_metadata.fast_small(sizeclass).slab_mask + 1;
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
    return sizeclass_metadata.slow(sizeclass_t::from_small_class(sizeclass))
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
    return sizeclass_metadata.slow(sizeclass_t::from_small_class(sizeclass))
      .capacity;
  }

  SNMALLOC_FAST_PATH constexpr size_t slab_index(sizeclass_t sc, address_t addr)
  {
    auto meta = sizeclass_metadata.fast(sc);
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

  SNMALLOC_FAST_PATH constexpr address_t
  start_of_object(sizeclass_t sc, address_t addr)
  {
    auto meta = sizeclass_metadata.fast(sc);
    address_t slab_start = addr & ~meta.slab_mask;
    size_t index = slab_index(sc, addr);
    return slab_start + (index * meta.size);
  }

  constexpr size_t index_in_object(sizeclass_t sc, address_t addr)
  {
    return addr - start_of_object(sc, addr);
  }

  constexpr size_t remaining_bytes(sizeclass_t sc, address_t addr)
  {
    return sizeclass_metadata.fast(sc).size - index_in_object(sc, addr);
  }

  constexpr bool is_start_of_object(sizeclass_t sc, address_t addr)
  {
    size_t offset = addr & (sizeclass_full_to_slab_size(sc) - 1);

    // Only works up to certain offsets, exhaustively tested by rounding.cc
    if constexpr (sizeof(offset) >= 8)
    {
      // Only works for 64 bit multiplication, as the following will overflow in
      // 32bit.
      // This is based on:
      //  https://lemire.me/blog/2019/02/20/more-fun-with-fast-remainders-when-the-divisor-is-a-constant/
      auto mod_zero_mult = sizeclass_metadata.fast(sc).mod_zero_mult;
      return (offset * mod_zero_mult) < mod_zero_mult;
    }
    else
      // Use 32-bit division as considerably faster than 64-bit, and
      // everything fits into 32bits here.
      return static_cast<uint32_t>(offset % sizeclass_full_to_size(sc)) == 0;
  }

  inline static size_t large_size_to_chunk_size(size_t size)
  {
    return bits::next_pow2(size);
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
             sizeclass_metadata.fast_small(smallsizeclass_t(sizeclass)).size;
             curr += MIN_ALLOC_STEP_SIZE)
        {
          table[sizeclass_lookup_index(curr)] = minimum_class;
        }
      }

      for (; sizeclass < NUM_SMALL_SIZECLASSES; sizeclass++)
      {
        for (; curr <=
             sizeclass_metadata.fast_small(smallsizeclass_t(sizeclass)).size;
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
   * Maps a requested size to its sizeclass. The result uses the unified
   * encoding documented on `sizeclass_t`.
   *
   * For small sizes, this delegates to `size_to_sizeclass`. For large
   * sizes in Phase 13, this rounds up to the next power of two (the
   * front end still requests pow2-rounded reservations); Phase 15
   * removes the `next_pow2` call to enable non-pow2 large reservations.
   *
   * `to_exp_mant` is the literal inverse of the `from_exp_mant` used
   * when populating `sizeclass_metadata`, so this never indexes the
   * wrong slot.
   */
  static inline sizeclass_t size_to_sizeclass_full(size_t size)
  {
    if (is_small_sizeclass(size))
    {
      return sizeclass_t::from_small_class(size_to_sizeclass(size));
    }
    SNMALLOC_ASSERT(size != 0);
    SNMALLOC_ASSERT(size <= MAX_LARGE_SIZECLASS_SIZE);
    size_t pow2 = bits::next_pow2(size);
    size_t global =
      bits::to_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(pow2);
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
    return bits::next_pow2(size);
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
