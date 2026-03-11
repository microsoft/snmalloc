#pragma once

#include "../ds/ds.h"

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
  using smallsizeclass_t = size_t;
  using chunksizeclass_t = size_t;

  static constexpr smallsizeclass_t size_to_sizeclass_const(size_t size)
  {
    // Don't use sizeclasses that are not a multiple of the alignment.
    // For example, 24 byte allocations can be
    // problematic for some data due to alignment issues.
    auto sc = static_cast<smallsizeclass_t>(
      bits::to_exp_mant_const<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(size));

    SNMALLOC_ASSERT(sc == static_cast<uint8_t>(sc));

    return sc;
  }

  constexpr size_t NUM_SMALL_SIZECLASSES =
    size_to_sizeclass_const(MAX_SMALL_SIZECLASS_SIZE) + 1;

  // Number of pow2-only large exponent levels (one per power of two).
  constexpr size_t NUM_LARGE_CLASSES_POW2 =
    DefaultPal::address_bits - MAX_SMALL_SIZECLASS_BITS;

  // Number of exponent levels that also get non-pow2 intermediate
  // sub-classes (mantissa 1..2^B-1).  Kept small enough that the
  // total large class count fits in TAG_SIZECLASS_BITS without
  // increasing REMOTE_MIN_ALIGN.  6 levels = cutoff at ~4 MiB.
  static constexpr size_t NUM_LARGE_FINE_LEVELS = 6;

  // Sub-classes per fine-grained level.
  static constexpr size_t LARGE_SUBS_PER_LEVEL =
    bits::one_at_bit(INTERMEDIATE_BITS);

  // The largest non-pow2 size class has S = (2^(B+1) - 1) * nat_align,
  // spanning (2^(B+1) - 1) nat_align blocks.  The last block's offset
  // is (2^(B+1) - 2), which must fit in the 3-bit pagemap offset field
  // (max value 7).  For B=2 the max offset is 6, which fits.
  static_assert(
    (bits::one_at_bit(INTERMEDIATE_BITS + 1) - 2) <= 7,
    "Non-pow2 large class offsets exceed 3-bit pagemap field capacity");

  // Total large class count: fine-grained levels contribute 2^B classes
  // each (including the pow2 at m=0), remaining levels contribute 1.
  constexpr size_t NUM_LARGE_CLASSES =
    NUM_LARGE_FINE_LEVELS * LARGE_SUBS_PER_LEVEL +
    (NUM_LARGE_CLASSES_POW2 - NUM_LARGE_FINE_LEVELS);

  // How many bits are required to represent either a large or a small
  // sizeclass.
  constexpr size_t TAG_SIZECLASS_BITS = bits::max<size_t>(
    bits::next_pow2_bits_const(NUM_SMALL_SIZECLASSES),
    bits::next_pow2_bits_const(NUM_LARGE_CLASSES + 1));

  // Number of bits required to represent a tagged sizeclass that can be
  // either small or large.
  constexpr size_t SIZECLASS_REP_SIZE =
    bits::one_at_bit(TAG_SIZECLASS_BITS + 1);

  /**
   * Encapsulates a tagged union of large and small sizeclasses.
   *
   * Used in various lookup tables to make efficient code that handles
   * all objects allocated by snmalloc.
   */
  class sizeclass_t
  {
    static constexpr size_t TAG = bits::one_at_bit(TAG_SIZECLASS_BITS);

    size_t value{0};

    constexpr sizeclass_t(size_t value) : value(value) {}

  public:
    constexpr sizeclass_t() = default;

    static constexpr sizeclass_t from_small_class(smallsizeclass_t sc)
    {
      SNMALLOC_ASSERT(sc < TAG);
      // Note could use `+` or `|`.  Using `+` as will combine nicely with array
      // offset.
      return {TAG + sc};
    }

    /**
     * Creates a sizeclass_t from a sequential large class index.
     * Indices 0..NUM_LARGE_CLASSES-1 enumerate all large classes:
     *   - Indices 0..(NUM_LARGE_FINE_LEVELS*LARGE_SUBS_PER_LEVEL-1) are
     *     fine-grained (including pow2 at mantissa 0 for each level).
     *   - The remaining indices are pow2-only classes.
     */
    static constexpr sizeclass_t from_large_class(size_t large_class)
    {
      // +1 reserves raw value 0 as a sentinel for uninitialized pagemap
      // entries, so that alloc_size(nullptr) returns 0.
      SNMALLOC_ASSERT(large_class + 1 < TAG);
      return {large_class + 1};
    }

    static constexpr sizeclass_t from_raw(size_t raw)
    {
      return {raw};
    }

    constexpr size_t index()
    {
      return value & (TAG - 1);
    }

    constexpr smallsizeclass_t as_small()
    {
      SNMALLOC_ASSERT(is_small());
      return value & (TAG - 1);
    }

    /**
     * Returns the sequential large class index.
     */
    constexpr size_t as_large()
    {
      SNMALLOC_ASSERT(!is_small());
      // -1 undoes the +1 offset from from_large_class.
      return (value & (TAG - 1)) - 1;
    }

    constexpr size_t raw()
    {
      return value;
    }

    constexpr bool is_small()
    {
      return (value & TAG) != 0;
    }

    constexpr bool is_default()
    {
      return value == 0;
    }

    constexpr bool operator==(sizeclass_t other)
    {
      return value == other.value;
    }
  };

  using sizeclass_compress_t = uint8_t;

  /**
   * The exp_mant index of the smallest large class in chunk units.
   * For B=2 with MAX_SMALL=64K and MIN_CHUNK=16K: 4 chunks → em index 3.
   */
  static constexpr size_t MIN_LARGE_EM =
    bits::to_exp_mant_const<INTERMEDIATE_BITS, 0>(
      MAX_SMALL_SIZECLASS_SIZE / MIN_CHUNK_SIZE);

  /**
   * Number of large class indices in the fine-grained range.
   * These levels have all 2^B sub-classes (m=0..3 for B=2).
   */
  static constexpr size_t LARGE_FINE_TOTAL =
    NUM_LARGE_FINE_LEVELS * LARGE_SUBS_PER_LEVEL;

  /**
   * Convert a sequential large class index to the byte size of the class.
   *
   * Indices 0..LARGE_FINE_TOTAL-1 cover fine-grained levels: each maps
   * directly to a consecutive exp_mant index starting from MIN_LARGE_EM.
   *
   * Indices >= LARGE_FINE_TOTAL cover pow2-only levels: each maps to
   * the pow2 (m=0) entry at the corresponding exponent level.
   */
  constexpr size_t large_class_index_to_size(size_t index)
  {
    size_t em_index;
    if (index < LARGE_FINE_TOTAL)
    {
      em_index = MIN_LARGE_EM + index;
    }
    else
    {
      size_t level = NUM_LARGE_FINE_LEVELS + (index - LARGE_FINE_TOTAL);
      em_index = MIN_LARGE_EM + level * LARGE_SUBS_PER_LEVEL;
    }
    size_t chunks = bits::from_exp_mant<INTERMEDIATE_BITS, 0>(em_index);
    return chunks * MIN_CHUNK_SIZE;
  }

  /**
   * Convert a byte size (which must be a valid large class size, i.e.
   * the result of round_size for a large allocation) to the sequential
   * large class index.
   */
  /**
   * Constexpr version for compile-time use (e.g. array sizing).
   */
  constexpr size_t size_to_large_class_index_const(size_t size)
  {
    size_t chunks = (size + MIN_CHUNK_SIZE - 1) / MIN_CHUNK_SIZE;
    size_t em = bits::to_exp_mant_const<INTERMEDIATE_BITS, 0>(chunks);
    size_t local_em = em - MIN_LARGE_EM;

    if (local_em < LARGE_FINE_TOTAL)
      return local_em;

    size_t m = local_em % LARGE_SUBS_PER_LEVEL;
    if (m != 0)
      local_em = (local_em / LARGE_SUBS_PER_LEVEL + 1) * LARGE_SUBS_PER_LEVEL;

    size_t level = local_em / LARGE_SUBS_PER_LEVEL;
    size_t index = LARGE_FINE_TOTAL + (level - NUM_LARGE_FINE_LEVELS);
    if (index >= NUM_LARGE_CLASSES)
      return NUM_LARGE_CLASSES - 1;
    return index;
  }

  /**
   * Runtime version using hardware CLZ intrinsic for fast path.
   */
  inline SNMALLOC_FAST_PATH size_t size_to_large_class_index(size_t size)
  {
    // Ceiling division to avoid truncation when size isn't chunk-aligned.
    size_t chunks = (size + MIN_CHUNK_SIZE - 1) / MIN_CHUNK_SIZE;
    size_t em = bits::to_exp_mant<INTERMEDIATE_BITS, 0>(chunks);
    size_t local_em = em - MIN_LARGE_EM;

    if (local_em < LARGE_FINE_TOTAL)
      return local_em;

    // Pow2-only range: if em corresponds to a non-pow2 mantissa,
    // round up to the next pow2 level.
    size_t m = local_em % LARGE_SUBS_PER_LEVEL;
    if (m != 0)
      local_em = (local_em / LARGE_SUBS_PER_LEVEL + 1) * LARGE_SUBS_PER_LEVEL;

    size_t level = local_em / LARGE_SUBS_PER_LEVEL;
    size_t index = LARGE_FINE_TOTAL + (level - NUM_LARGE_FINE_LEVELS);
    // Clamp to valid range for sizes beyond the representable address space.
    if (index >= NUM_LARGE_CLASSES)
      return NUM_LARGE_CLASSES - 1;
    return index;
  }

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
      size_t max_capacity = 0;

      for (sizeclass_compress_t sizeclass = 0;
           sizeclass < NUM_SMALL_SIZECLASSES;
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

      for (sizeclass_compress_t sizeclass = 0;
           sizeclass < NUM_SMALL_SIZECLASSES;
           sizeclass++)
      {
        // Calculate reciprocal division constant.
        auto& meta = fast_small(sizeclass);
        meta.div_mult = (bits::mask_bits(DIV_MULT_SHIFT) / meta.size) + 1;

        size_t zero = 0;
        meta.mod_zero_mult = (~zero / meta.size) + 1;
      }

      for (size_t index = 0; index < NUM_LARGE_CLASSES; index++)
      {
        auto lsc = sizeclass_t::from_large_class(index);
        auto& meta = fast(lsc);
        meta.size = large_class_index_to_size(index);
        // For pow2 large classes: slab_mask = size - 1.
        // For non-pow2 large classes: slab_mask = natural_alignment(size) - 1.
        // Each pagemap chunk entry stores its distance (in nat_align
        // units) from the allocation start, enabling start_of_object
        // to work for any interior pointer without placement constraints.
        size_t nat = meta.size & (~(meta.size - 1));
        meta.slab_mask = nat - 1;
        // The slab_mask will do all the necessary work, so
        // perform identity multiplication for the test.
        meta.mod_zero_mult = 1;
        // The slab_mask will do all the necessary work for division
        // so collapse the calculated offset.
        meta.div_mult = 0;
      }

      // Raw sizeclass value 0 is the sentinel for uninitialized pagemap
      // entries (non-snmalloc memory like stack, globals, etc.).
      // Set size=0 so alloc_size(nullptr) returns 0, and
      // slab_mask=SIZE_MAX so remaining_bytes returns a large value,
      // preventing false-positive bounds check failures on non-heap
      // addresses.
      {
        auto& sentinel = fast_[0];
        sentinel.size = 0;
        size_t zero = 0;
        sentinel.slab_mask = ~zero;
        sentinel.mod_zero_mult = 1;
        sentinel.div_mult = 0;
      }
    }
  };

  constexpr SizeClassTable sizeclass_metadata = SizeClassTable();

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

  constexpr uint16_t sizeclass_to_slab_object_count(smallsizeclass_t sizeclass)
  {
    return sizeclass_metadata.slow(sizeclass_t::from_small_class(sizeclass))
      .capacity;
  }

  SNMALLOC_FAST_PATH constexpr size_t
  slab_index(sizeclass_t sc, address_t addr, uint8_t offset_bits = 0)
  {
    auto meta = sizeclass_metadata.fast(sc);
    size_t offset = addr & meta.slab_mask;
    // For non-pow2 large allocations, offset_bits records this
    // pagemap entry's distance from the allocation start, measured
    // in nat_align (= slab_mask + 1) units.  Adding it reconstructs
    // the total byte offset from the start of the allocation.
    // For small and pow2-large sizeclasses offset_bits is always 0,
    // so this addition is a no-op and the fast path is unchanged.
    offset += (meta.slab_mask + 1) * offset_bits;
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
  start_of_object(sizeclass_t sc, address_t addr, uint8_t offset_bits = 0)
  {
    auto meta = sizeclass_metadata.fast(sc);
    address_t nat_base = addr & ~meta.slab_mask;
    // Subtract offset_bits * nat_align to recover the allocation start.
    // Each pagemap entry stores how many nat_align blocks it is from
    // the start, so subtracting walks back to the beginning.
    // For small and pow2-large sizeclasses offset_bits is always 0,
    // so alloc_start == nat_base and the fast path is unchanged.
    address_t alloc_start = nat_base - (meta.slab_mask + 1) * offset_bits;
    size_t index = slab_index(sc, addr, offset_bits);
    return alloc_start + (index * meta.size);
  }

  constexpr size_t
  index_in_object(sizeclass_t sc, address_t addr, uint8_t offset_bits = 0)
  {
    return addr - start_of_object(sc, addr, offset_bits);
  }

  constexpr size_t
  remaining_bytes(sizeclass_t sc, address_t addr, uint8_t offset_bits = 0)
  {
    return sizeclass_metadata.fast(sc).size -
      index_in_object(sc, addr, offset_bits);
  }

  constexpr bool
  is_start_of_object(sizeclass_t sc, address_t addr, uint8_t offset_bits = 0)
  {
    size_t offset = addr & (sizeclass_full_to_slab_size(sc) - 1);
    // Branchless offset correction: add the entry's distance from the
    // allocation start (in nat_align units) to get the total offset.
    offset += sizeclass_full_to_slab_size(sc) * offset_bits;

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
    return large_class_index_to_size(size_to_large_class_index(size));
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
        for (; curr <= sizeclass_metadata.fast_small(sizeclass).size;
             curr += MIN_ALLOC_STEP_SIZE)
        {
          table[sizeclass_lookup_index(curr)] = minimum_class;
        }
      }

      for (; sizeclass < NUM_SMALL_SIZECLASSES; sizeclass++)
      {
        for (; curr <= sizeclass_metadata.fast_small(sizeclass).size;
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

  /**
   * @brief Returns true if the size is a small sizeclass. Note that
   * 0 is not considered a small sizeclass.
   */
  constexpr bool is_small_sizeclass(size_t size)
  {
    // Perform the - 1 on size, so that zero wraps around and ends up on
    // slow path.
    return (size - 1) < sizeclass_to_size(NUM_SMALL_SIZECLASSES - 1);
  }

  constexpr smallsizeclass_t size_to_sizeclass(size_t size)
  {
    if (SNMALLOC_LIKELY(is_small_sizeclass(size)))
    {
      auto index = sizeclass_lookup_index(size);
      SNMALLOC_ASSERT(index < sizeclass_lookup_size);
      return sizeclass_lookup.table[index];
    }

    // Check this is not called on large sizes.
    SNMALLOC_ASSERT(size == 0);
    // Map size == 0 to the first sizeclass.
    return 0;
  }

  /**
   * A compressed size representation,
   *   either a small size class with TAG bit set
   *   or a large class with TAG bit not set.
   * Large classes use a sequential index; see large_class_index_to_size.
   */
  static inline sizeclass_t size_to_sizeclass_full(size_t size)
  {
    if (is_small_sizeclass(size))
    {
      return sizeclass_t::from_small_class(size_to_sizeclass(size));
    }
    // For large sizes, compute the sequential large class index.
    // size_to_large_class_index rounds up to the next valid class.
    return sizeclass_t::from_large_class(size_to_large_class_index(size));
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

    if (size > bits::one_at_bit(bits::BITS - 1))
    {
      // This size is too large, no rounding should occur as will result in a
      // failed allocation later.
      return size;
    }
    // Use fine-grained large size classes (B=2 intermediate classes)
    // for sizes within the fine-grained range, pow2 for larger.
    return large_class_index_to_size(size_to_large_class_index(size));
  }

  /// Returns the alignment that this size naturally has, that is
  /// all allocations of size `size` will be aligned to the returned value.
  inline SNMALLOC_FAST_PATH static size_t natural_alignment(size_t size)
  {
    if (size == 0)
      return 1;
    return bits::one_at_bit(bits::ctz(size));
  }

  constexpr SNMALLOC_FAST_PATH static size_t
  aligned_size(size_t alignment, size_t size)
  {
    // Client responsible for checking alignment is not zero
    SNMALLOC_ASSERT(alignment != 0);
    // Client responsible for checking alignment is a power of two
    SNMALLOC_ASSERT(bits::is_pow2(alignment));

    // There are a class of corner cases to consider
    //    alignment = 0x8
    //    size = 0xfff...fff7
    // for this result will be 0.  This should fail an allocation, so we need to
    // check for this overflow.
    // However,
    //    alignment = 0x8
    //    size      = 0x0
    // will also result in 0, but this should be allowed to allocate.
    // So we need to check for overflow, and return SIZE_MAX in this first case,
    // and 0 in the second.
    size_t result = ((alignment - 1) | (size - 1)) + 1;
    // The following code is designed to fuse well with a subsequent
    // sizeclass calculation.  We use the same fast path constant to
    // move the case where result==0 to the slow path, and then check for which
    // case we are in.
    if (is_small_sizeclass(result))
      return result;

    // We are in the slow path, so we need to check for overflow.
    if (SNMALLOC_UNLIKELY(result == 0))
    {
      // Check for overflow and return the maximum size.
      if (SNMALLOC_UNLIKELY(result < size))
        return SIZE_MAX;
    }
    return result;
  }
} // namespace snmalloc
