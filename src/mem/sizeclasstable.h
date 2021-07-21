#pragma once

#include "../ds/bits.h"
#include "../ds/defines.h"
#include "../ds/helpers.h"
#include "allocconfig.h"

namespace snmalloc
{
  // Both usings should compile
  // We use size_t as it generates better code.
  using sizeclass_t = size_t;
  //  using sizeclass_t = uint8_t;
  using sizeclass_compress_t = uint8_t;

  constexpr static uintptr_t SIZECLASS_MASK = 0xFF;

  constexpr static inline sizeclass_t size_to_sizeclass_const(size_t size)
  {
    // Don't use sizeclasses that are not a multiple of the alignment.
    // For example, 24 byte allocations can be
    // problematic for some data due to alignment issues.
    auto sc = static_cast<sizeclass_t>(
      bits::to_exp_mant_const<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(size));

    SNMALLOC_ASSERT(sc == static_cast<uint8_t>(sc));

    return sc;
  }

  static inline size_t large_sizeclass_to_size(uint8_t large_class)
  {
    // TODO. Remove
    UNUSED(large_class);
    abort();
    //    return bits::one_at_bit(large_class + SUPERSLAB_BITS);
  }

  static constexpr size_t NUM_SIZECLASSES =
    size_to_sizeclass_const(MAX_SIZECLASS_SIZE);

  // Large classes range from [SUPERSLAB, ADDRESS_SPACE).// TODO
  static constexpr size_t NUM_LARGE_CLASSES =
    Pal::address_bits - MAX_SIZECLASS_BITS;

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
    size_t mod_mult;
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
    ModArray<NUM_SIZECLASSES, sizeclass_data_fast> fast;
    ModArray<NUM_SIZECLASSES, sizeclass_data_slow> slow;

    constexpr SizeClassTable() : fast(), slow()
    {
      for (sizeclass_compress_t sizeclass = 0; sizeclass < NUM_SIZECLASSES;
           sizeclass++)
      {
        size_t rsize =
          bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(sizeclass);
        fast[sizeclass].size = rsize;
        size_t slab_bits = bits::max(
          bits::next_pow2_bits_const(MIN_OBJECT_COUNT * rsize), MIN_CHUNK_BITS);

        fast[sizeclass].slab_mask = bits::one_at_bit(slab_bits) - 1;

        slow[sizeclass].capacity =
          static_cast<uint16_t>((fast[sizeclass].slab_mask + 1) / rsize);

        slow[sizeclass].waking =
#ifdef SNMALLOC_CHECK_CLIENT
          static_cast<uint16_t>(slow[sizeclass].capacity / 4);
#else
          static_cast<uint16_t>(bits::min((slow[sizeclass].capacity / 4), 32));
#endif
      }

      for (sizeclass_compress_t sizeclass = 0; sizeclass < NUM_SIZECLASSES;
           sizeclass++)
      {
        fast[sizeclass].div_mult = // TODO is MAX_SIZECLASS_BITS right?
          (bits::one_at_bit(bits::BITS - 24) /
           (fast[sizeclass].size / MIN_ALLOC_SIZE));
        if (!bits::is_pow2(fast[sizeclass].size))
          fast[sizeclass].div_mult++;

        fast[sizeclass].mod_mult =
          (bits::one_at_bit(bits::BITS - 1) / fast[sizeclass].size);
        if (!bits::is_pow2(fast[sizeclass].size))
          fast[sizeclass].mod_mult++;
        // Shift multiplier, so that the result of division completely
        // overflows, and thus the top SUPERSLAB_BITS will be zero if the mod is
        // zero.
        fast[sizeclass].mod_mult *= 2;
      }
    }
  };

  static inline constexpr SizeClassTable sizeclass_metadata = SizeClassTable();

  constexpr static inline size_t sizeclass_to_size(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.fast[sizeclass].size;
  }

  inline static size_t sizeclass_to_slab_size(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.fast[sizeclass].slab_mask + 1;
  }

  /**
   * Only wake slab if we have this many free allocations
   *
   * This helps remove bouncing around empty to non-empty cases.
   *
   * It also increases entropy, when we have randomisation.
   */
  inline uint16_t threshold_for_waking_slab(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.slow[sizeclass].waking;
  }

  inline static size_t sizeclass_to_slab_sizeclass(sizeclass_t sizeclass)
  {
    size_t ssize = sizeclass_to_slab_size(sizeclass);

    return bits::next_pow2_bits(ssize) - MIN_CHUNK_BITS;
  }

  inline static size_t slab_sizeclass_to_size(sizeclass_t sizeclass)
  {
    return bits::one_at_bit(MIN_CHUNK_BITS + sizeclass);
  }

  inline constexpr static uint16_t
  sizeclass_to_slab_object_count(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.slow[sizeclass].capacity;
  }

  inline static size_t round_by_sizeclass(sizeclass_t sc, size_t offset)
  {
    // Only works up to certain offsets, exhaustively tested upto
    // SUPERSLAB_SIZE.
    //    SNMALLOC_ASSERT(offset <= SUPERSLAB_SIZE);

    auto rsize = sizeclass_to_size(sc);

    if constexpr (sizeof(offset) >= 8)
    {
      // Only works for 64 bit multiplication, as the following will overflow in
      // 32bit.
      // The code is using reciprocal division.  If SUPERSLABS
      // get larger then we should review this code. For 24 bits, there are in
      // sufficient bits to do this completely efficiently as 24 * 3 is larger
      // than 64 bits.  But we can pre-round by MIN_ALLOC_SIZE which gets us an
      // extra 4 * 3 bits, and thus achievable in 64bit multiplication.
      // static_assert(
      //   SUPERSLAB_BITS <= 24, "The following code assumes max of 24 bits");

      // TODO 24 hack
      static_assert(bits::BITS >= 24, "About to attempt a negative shift");
      static_assert(
        (8 * sizeof(offset)) >= (bits::BITS - 24),
        "About to shift further than the type");
      return (((offset >> MIN_ALLOC_BITS) *
               sizeclass_metadata.fast[sc].div_mult) >>
              (bits::BITS - 24)) *
        rsize;
    }
    else
      // Use 32-bit division as considerably faster than 64-bit, and
      // everything fits into 32bits here.
      return static_cast<uint32_t>(offset / rsize) * rsize;
  }

  inline static bool is_multiple_of_sizeclass(sizeclass_t sc, size_t offset)
  {
    // Only works up to certain offsets, exhaustively tested upto
    // SUPERSLAB_SIZE.
    //    SNMALLOC_ASSERT(offset <= SUPERSLAB_SIZE);

    if constexpr (sizeof(offset) >= 8)
    {
      // Only works for 64 bit multiplication, as the following will overflow in
      // 32bit.
      // The code is using reciprocal division.  If SUPERSLABS
      // get larger then we should review this code.  The modulus code
      // has fewer restrictions than division, as it only requires the
      // square of the offset to be representable.
      // TODO 24 hack. Redo the maths given the multiple
      // slab sizes
      static_assert(bits::BITS >= 25);
      static constexpr size_t MASK =
        ~(bits::one_at_bit(bits::BITS - 1 - 24) - 1);

      return ((offset * sizeclass_metadata.fast[sc].mod_mult) & MASK) == 0;
    }
    else
      // Use 32-bit division as considerably faster than 64-bit, and
      // everything fits into 32bits here.
      return static_cast<uint32_t>(offset % sizeclass_to_size(sc)) == 0;
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

  static inline sizeclass_t size_to_sizeclass(size_t size)
  {
    constexpr static size_t sizeclass_lookup_size =
      sizeclass_lookup_index(MAX_SIZECLASS_SIZE);

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
        for (sizeclass_compress_t sizeclass = 0; sizeclass < NUM_SIZECLASSES;
             sizeclass++)
        {
          for (; curr <= sizeclass_metadata.fast[sizeclass].size;
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

  inline SNMALLOC_FAST_PATH static size_t round_size(size_t size)
  {
    if (size > sizeclass_to_size(NUM_SIZECLASSES - 1))
    {
      return bits::next_pow2(size);
    }
    if (size == 0)
    {
      return 0;
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
