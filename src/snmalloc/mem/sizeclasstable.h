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

  constexpr static inline smallsizeclass_t size_to_sizeclass_const(size_t size)
  {
    // Don't use sizeclasses that are not a multiple of the alignment.
    // For example, 24 byte allocations can be
    // problematic for some data due to alignment issues.
    auto sc = static_cast<smallsizeclass_t>(
      bits::to_exp_mant_const<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(size));

    SNMALLOC_ASSERT(sc == static_cast<uint8_t>(sc));

    return sc;
  }

  static constexpr size_t NUM_SMALL_SIZECLASSES =
    size_to_sizeclass_const(MAX_SMALL_SIZECLASS_SIZE);

  // Large classes range from [MAX_SMALL_SIZECLASS_SIZE, ADDRESS_SPACE).
  static constexpr size_t NUM_LARGE_CLASSES =
    Pal::address_bits - MAX_SMALL_SIZECLASS_BITS;

  // How many bits are required to represent either a large or a small
  // sizeclass.
  static constexpr size_t TAG_SIZECLASS_BITS = bits::max<size_t>(
    bits::next_pow2_bits_const(NUM_SMALL_SIZECLASSES + 1),
    bits::next_pow2_bits_const(NUM_LARGE_CLASSES + 1));

  // Number of bits required to represent a tagged sizeclass that can be
  // either small or large.
  static constexpr size_t SIZECLASS_REP_SIZE =
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

    constexpr static sizeclass_t from_small_class(smallsizeclass_t sc)
    {
      SNMALLOC_ASSERT(sc < TAG);
      // Note could use `+` or `|`.  Using `+` as will combine nicely with array
      // offset.
      return {TAG + sc};
    }

    /**
     * Takes the number of leading zero bits from the actual large size-1.
     * See size_to_sizeclass_full
     */
    constexpr static sizeclass_t from_large_class(size_t large_class)
    {
      SNMALLOC_ASSERT(large_class < TAG);
      return {large_class};
    }

    constexpr static sizeclass_t from_raw(size_t raw)
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

    constexpr chunksizeclass_t as_large()
    {
      SNMALLOC_ASSERT(!is_small());
      return bits::BITS - (value & (TAG - 1));
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

  inline SNMALLOC_FAST_PATH static size_t
  aligned_size(size_t alignment, size_t size)
  {
    // Client responsible for checking alignment is not zero
    SNMALLOC_ASSERT(alignment != 0);
    // Client responsible for checking alignment is a power of two
    SNMALLOC_ASSERT(bits::is_pow2(alignment));

    return ((alignment - 1) | (size - 1)) + 1;
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

  struct SizeClassTable
  {
    ModArray<SIZECLASS_REP_SIZE, sizeclass_data_fast> fast_;
    ModArray<SIZECLASS_REP_SIZE, sizeclass_data_slow> slow_;

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

    constexpr SizeClassTable() : fast_(), slow_(), DIV_MULT_SHIFT()
    {
      size_t max_capacity = 0;

      for (sizeclass_compress_t sizeclass = 0;
           sizeclass < NUM_SMALL_SIZECLASSES;
           sizeclass++)
      {
        auto& meta = fast_small(sizeclass);

        size_t rsize =
          bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(sizeclass);
        meta.size = rsize;
        size_t slab_bits = bits::max(
          bits::next_pow2_bits_const(MIN_OBJECT_COUNT * rsize), MIN_CHUNK_BITS);

        meta.slab_mask = bits::one_at_bit(slab_bits) - 1;

        auto& meta_slow = slow(sizeclass_t::from_small_class(sizeclass));
        meta_slow.capacity =
          static_cast<uint16_t>((meta.slab_mask + 1) / rsize);

        meta_slow.waking =
#ifdef SNMALLOC_CHECK_CLIENT
          static_cast<uint16_t>(meta_slow.capacity / 4);
#else
          static_cast<uint16_t>(bits::min((meta_slow.capacity / 4), 32));
#endif

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
        meta.div_mult =
          ((bits::one_at_bit(DIV_MULT_SHIFT) - 1) / meta.size) + 1;

        size_t zero = 0;
        meta.mod_zero_mult = (~zero / meta.size) + 1;
      }

      for (size_t sizeclass = 0; sizeclass < bits::BITS; sizeclass++)
      {
        auto lsc = sizeclass_t::from_large_class(sizeclass);
        auto& meta = fast(lsc);
        meta.size = sizeclass == 0 ? 0 : bits::one_at_bit(lsc.as_large());
        meta.slab_mask = meta.size - 1;
        // The slab_mask will do all the necessary work, so
        // perform identity multiplication for the test.
        meta.mod_zero_mult = 1;
        // The slab_mask will do all the necessary work for division
        // so collapse the calculated offset.
        meta.div_mult = 0;
      }
    }
  };

  static inline constexpr SizeClassTable sizeclass_metadata = SizeClassTable();

  static constexpr size_t DIV_MULT_SHIFT = sizeclass_metadata.DIV_MULT_SHIFT;

  constexpr static inline size_t sizeclass_to_size(smallsizeclass_t sizeclass)
  {
    return sizeclass_metadata.fast_small(sizeclass).size;
  }

  static inline size_t sizeclass_full_to_size(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.fast(sizeclass).size;
  }

  inline static size_t sizeclass_full_to_slab_size(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.fast(sizeclass).slab_mask + 1;
  }

  inline static size_t sizeclass_to_slab_size(smallsizeclass_t sizeclass)
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
  inline uint16_t threshold_for_waking_slab(smallsizeclass_t sizeclass)
  {
    return sizeclass_metadata.slow(sizeclass_t::from_small_class(sizeclass))
      .waking;
  }

  inline static size_t sizeclass_to_slab_sizeclass(smallsizeclass_t sizeclass)
  {
    size_t ssize = sizeclass_to_slab_size(sizeclass);

    return bits::next_pow2_bits(ssize) - MIN_CHUNK_BITS;
  }

  inline static size_t slab_sizeclass_to_size(chunksizeclass_t sizeclass)
  {
    return bits::one_at_bit(MIN_CHUNK_BITS + sizeclass);
  }

  /**
   * For large allocations, the metaentry stores the raw log_2 of the size,
   * which must be shifted into the index space of slab_sizeclass-es.
   */
  inline static size_t
  metaentry_chunk_sizeclass_to_slab_sizeclass(chunksizeclass_t sizeclass)
  {
    return sizeclass - MIN_CHUNK_BITS;
  }

  inline constexpr static uint16_t
  sizeclass_to_slab_object_count(smallsizeclass_t sizeclass)
  {
    return sizeclass_metadata.slow(sizeclass_t::from_small_class(sizeclass))
      .capacity;
  }

  inline static address_t start_of_object(sizeclass_t sc, address_t addr)
  {
    auto meta = sizeclass_metadata.fast(sc);
    address_t slab_start = addr & ~meta.slab_mask;
    size_t offset = addr & meta.slab_mask;
    size_t size = meta.size;

    if constexpr (sizeof(addr) >= 8)
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
      size_t offset_start = ((offset * meta.div_mult) >> DIV_MULT_SHIFT) * size;
      return slab_start + offset_start;
    }
    else
    {
      if (size == 0)
        return 0;
      return slab_start + (offset / size) * size;
    }
  }

  inline static size_t index_in_object(sizeclass_t sc, address_t addr)
  {
    return addr - start_of_object(sc, addr);
  }

  inline static size_t remaining_bytes(sizeclass_t sc, address_t addr)
  {
    return sizeclass_metadata.fast(sc).size - index_in_object(sc, addr);
  }

  inline static bool is_start_of_object(sizeclass_t sc, address_t addr)
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

  inline static size_t large_size_to_chunk_sizeclass(size_t size)
  {
    return bits::next_pow2_bits(size) - MIN_CHUNK_BITS;
  }

  constexpr static SNMALLOC_PURE size_t sizeclass_lookup_index(const size_t s)
  {
    // We subtract and shift to reduce the size of the table, i.e. we don't have
    // to store a value for every size.
    return (s - 1) >> MIN_ALLOC_BITS;
  }

  static inline smallsizeclass_t size_to_sizeclass(size_t size)
  {
    constexpr static size_t sizeclass_lookup_size =
      sizeclass_lookup_index(MAX_SMALL_SIZECLASS_SIZE);

    /**
     * This struct is used to statically initialise a table for looking up
     * the correct sizeclass.
     */
    struct SizeClassLookup
    {
      sizeclass_compress_t table[sizeclass_lookup_size] = {{}};

      constexpr SizeClassLookup()
      {
        size_t curr = 1;
        for (sizeclass_compress_t sizeclass = 0;
             sizeclass < NUM_SMALL_SIZECLASSES;
             sizeclass++)
        {
          for (; curr <= sizeclass_metadata.fast_small(sizeclass).size;
               curr += 1 << MIN_ALLOC_BITS)
          {
            auto i = sizeclass_lookup_index(curr);
            if (i == sizeclass_lookup_size)
              break;
            table[i] = sizeclass;
          }
        }
      }
    };

    static constexpr SizeClassLookup sizeclass_lookup = SizeClassLookup();

    auto index = sizeclass_lookup_index(size);
    if (index < sizeclass_lookup_size)
    {
      return sizeclass_lookup.table[index];
    }

    // Check this is not called on large sizes.
    SNMALLOC_ASSERT(size == 0);
    // Map size == 0 to the first sizeclass.
    return 0;
  }

  /**
   * A compressed size representation,
   *   either a small size class with the 7th bit set
   *   or a large class with the 7th bit not set.
   * Large classes are stored as a mask shift.
   *    size = (~0 >> lc) + 1;
   * Thus large size class 0, has size 0.
   * And large size class 33, has size 2^31
   */
  static inline sizeclass_t size_to_sizeclass_full(size_t size)
  {
    if ((size - 1) < sizeclass_to_size(NUM_SMALL_SIZECLASSES - 1))
    {
      return sizeclass_t::from_small_class(size_to_sizeclass(size));
    }
    // bits::clz is undefined on 0, but we have size == 1 has already been
    // handled here.  We conflate 0 and sizes larger than we can allocate.
    return sizeclass_t::from_large_class(bits::clz(size - 1));
  }

  inline SNMALLOC_FAST_PATH static size_t round_size(size_t size)
  {
    if (size > sizeclass_to_size(NUM_SMALL_SIZECLASSES - 1))
    {
      return bits::next_pow2(size);
    }
    // If realloc(ptr, 0) returns nullptr, some consumers treat this as a
    // reallocation failure and abort.  To avoid this, we round up the size of
    // requested allocations to the smallest size class.  This can be changed
    // on any platform that's happy to return nullptr from realloc(ptr,0) and
    // should eventually become a configuration option.
    if (size == 0)
    {
      return sizeclass_to_size(size_to_sizeclass(1));
    }
    return sizeclass_to_size(size_to_sizeclass(size));
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
