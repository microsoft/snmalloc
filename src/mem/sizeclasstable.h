#pragma once

#include "../ds/bits.h"
#include "../ds/helpers.h"
#include "sizeclass.h"

namespace snmalloc
{
  constexpr static SNMALLOC_PURE size_t sizeclass_lookup_index(const size_t s)
  {
    // We subtract and shift to reduce the size of the table, i.e. we don't have
    // to store a value for every size.
    return (s - 1) >> MIN_ALLOC_BITS;
  }

  constexpr static size_t NUM_SIZECLASSES_EXTENDED =
    size_to_sizeclass_const(bits::one_at_bit(bits::ADDRESS_BITS));

  constexpr static size_t sizeclass_lookup_size =
    sizeclass_lookup_index(MAX_SIZECLASS_SIZE);

  struct SizeClassTable
  {
    sizeclass_compress_t sizeclass_lookup[sizeclass_lookup_size] = {{}};
    ModArray<NUM_SIZECLASSES_EXTENDED, size_t> size;

    ModArray<NUM_SIZECLASSES, uint16_t> capacity;
    ModArray<NUM_SIZECLASSES, uint16_t> waking;
    ModArray<NUM_SIZECLASSES, size_t> slab_size;

    // Table of constants for reciprocal division for each sizeclass.
    ModArray<NUM_SIZECLASSES, size_t> div_mult;
    // Table of constants for reciprocal modulus for each sizeclass.
    ModArray<NUM_SIZECLASSES, size_t> mod_mult;

    constexpr SizeClassTable()
    : size(), capacity(), waking(), slab_size(), div_mult(), mod_mult()
    {
      for (sizeclass_compress_t sizeclass = 0; sizeclass < NUM_SIZECLASSES;
           sizeclass++)
      {
        size_t rsize =
          bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(sizeclass);
        size[sizeclass] = rsize;
        size_t slab_bits = bits::max(
          bits::next_pow2_bits_const(MIN_OBJECT_COUNT * rsize), MIN_CHUNK_BITS);

        slab_size[sizeclass] = bits::one_at_bit(slab_bits);

        capacity[sizeclass] = (uint16_t)(slab_size[sizeclass] / rsize);

        waking[sizeclass] =
          (uint16_t)bits::min((capacity[sizeclass] / 16) + 2, 32);
      }

      for (sizeclass_compress_t sizeclass = NUM_SIZECLASSES;
           sizeclass < NUM_SIZECLASSES_EXTENDED;
           sizeclass++)
      {
        size[sizeclass] = bits::prev_pow2_const(
          bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(sizeclass));
      }

      for (sizeclass_compress_t sizeclass = 0; sizeclass < NUM_SIZECLASSES;
           sizeclass++)
      {
        div_mult[sizeclass] = // TODO is MAX_SIZECLASS_BITS right?
          (bits::one_at_bit(bits::BITS - 24) /
           (size[sizeclass] / MIN_ALLOC_SIZE));
        if (!bits::is_pow2(size[sizeclass]))
          div_mult[sizeclass]++;

        mod_mult[sizeclass] =
          (bits::one_at_bit(bits::BITS - 1) / size[sizeclass]);
        if (!bits::is_pow2(size[sizeclass]))
          mod_mult[sizeclass]++;
        // Shift multiplier, so that the result of division completely
        // overflows, and thus the top SUPERSLAB_BITS will be zero if the mod is
        // zero.
        mod_mult[sizeclass] *= 2;
      }

      size_t curr = 1;
      for (sizeclass_compress_t sizeclass = 0; sizeclass <= NUM_SIZECLASSES;
           sizeclass++)
      {
        for (; curr <= size[sizeclass]; curr += 1 << MIN_ALLOC_BITS)
        {
          auto i = sizeclass_lookup_index(curr);
          if (i == sizeclass_lookup_size)
            break;
          sizeclass_lookup[i] = sizeclass;
        }
      }
    }
  };

  static inline constexpr SizeClassTable sizeclass_metadata = SizeClassTable();

  constexpr static inline size_t sizeclass_to_size(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.size[sizeclass];
  }

  inline static size_t sizeclass_to_slab_size(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.slab_size[sizeclass];
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
// #ifdef CHECK_CLIENT
    return sizeclass_metadata.waking[sizeclass];
// #else
//     UNUSED(sizeclass);
//     return 1;
// #endif
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

  inline static uint16_t sizeclass_to_slab_object_count(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.capacity[sizeclass];
  }

  static inline sizeclass_t size_to_sizeclass(size_t size)
  {
    auto index = sizeclass_lookup_index(size);
    if (index < sizeclass_lookup_size)
    {
      return sizeclass_metadata.sizeclass_lookup[index];
    }

    // Don't use sizeclasses that are not a multiple of the alignment.
    // For example, 24 byte allocations can be
    // problematic for some data due to alignment issues.

    // TODO hack to power of 2 for large sizes
    size = bits::next_pow2(size);

    return static_cast<sizeclass_t>(
      bits::to_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(size));
  }

  inline static size_t round_by_sizeclass(sizeclass_t sc, size_t offset)
  {
    // Only works up to certain offsets, exhaustively tested upto
    // SUPERSLAB_SIZE.
    //    SNMALLOC_ASSERT(offset <= SUPERSLAB_SIZE);

    auto rsize = sizeclass_to_size(sc);

    if constexpr (bits::is64())
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
      return (((offset >> MIN_ALLOC_BITS) * sizeclass_metadata.div_mult[sc]) >>
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

    if constexpr (bits::is64())
    {
      // Only works for 64 bit multiplication, as the following will overflow in
      // 32bit.
      // The code is using reciprocal division.  If SUPERSLABS
      // get larger then we should review this code.  The modulus code
      // has fewer restrictions than division, as it only requires the
      // square of the offset to be representable.
      // TODO 24 hack. Redo the maths given the multiple
      // slab sizes
      static constexpr size_t MASK =
        ~(bits::one_at_bit(bits::BITS - 1 - 24) - 1);

      return ((offset * sizeclass_metadata.mod_mult[sc]) & MASK) == 0;
    }
    else
      // Use 32-bit division as considerably faster than 64-bit, and
      // everything fits into 32bits here.
      return static_cast<uint32_t>(offset % sizeclass_to_size(sc)) == 0;
  }

} // namespace snmalloc
