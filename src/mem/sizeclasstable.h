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
    UNUSED(large_class);
    abort();
    //    return bits::one_at_bit(large_class + SUPERSLAB_BITS);
  }

  static constexpr size_t NUM_SIZECLASSES =
    size_to_sizeclass_const(MAX_SIZECLASS_SIZE);

  // Large classes range from [SUPERSLAB, ADDRESS_SPACE).// TODO
  static constexpr size_t NUM_LARGE_CLASSES =
    bits::ADDRESS_BITS - MAX_SIZECLASS_BITS;

  inline SNMALLOC_FAST_PATH static size_t
  aligned_size(size_t alignment, size_t size)
  {
    // Client responsible for checking alignment is not zero
    SNMALLOC_ASSERT(alignment != 0);
    // Client responsible for checking alignment is a power of two
    SNMALLOC_ASSERT(bits::is_pow2(alignment));

    return ((alignment - 1) | (size - 1)) + 1;
  }

  constexpr static SNMALLOC_PURE size_t sizeclass_lookup_index(const size_t s)
  {
    // We subtract and shift to reduce the size of the table, i.e. we don't have
    // to store a value for every size.
    return (s - 1) >> MIN_ALLOC_BITS;
  }

  constexpr static size_t NUM_SIZECLASSES_EXTENDED =
    size_to_sizeclass_const(bits::one_at_bit(bits::ADDRESS_BITS - 1));

  constexpr static size_t sizeclass_lookup_size =
    sizeclass_lookup_index(MAX_SIZECLASS_SIZE);

  struct SizeClassTable
  {
    sizeclass_compress_t sizeclass_lookup[sizeclass_lookup_size] = {{}};
    ModArray<NUM_SIZECLASSES_EXTENDED, size_t> size;

    ModArray<NUM_SIZECLASSES, uint16_t> capacity;
    ModArray<NUM_SIZECLASSES, uint16_t> waking;
    // We store the mask as it is used more on the fast path, and the size of
    // the slab.
    ModArray<NUM_SIZECLASSES, size_t> slab_mask;

    // Table of constants for reciprocal division for each sizeclass.
    ModArray<NUM_SIZECLASSES, size_t> div_mult;
    // Table of constants for reciprocal modulus for each sizeclass.
    ModArray<NUM_SIZECLASSES, size_t> mod_mult;

    constexpr SizeClassTable()
    : size(), capacity(), waking(), slab_mask(), div_mult(), mod_mult()
    {
      for (sizeclass_compress_t sizeclass = 0; sizeclass < NUM_SIZECLASSES;
           sizeclass++)
      {
        size_t rsize =
          bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(sizeclass);
        size[sizeclass] = rsize;
        size_t slab_bits = bits::max(
          bits::next_pow2_bits_const(MIN_OBJECT_COUNT * rsize), MIN_CHUNK_BITS);

        slab_mask[sizeclass] = bits::one_at_bit(slab_bits) - 1;

        capacity[sizeclass] = (uint16_t)((slab_mask[sizeclass] + 1) / rsize);

        waking[sizeclass] = (uint16_t)bits::min((capacity[sizeclass] / 4), 32);
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
    return sizeclass_metadata.slab_mask[sizeclass] + 1;
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

  inline constexpr static uint16_t
  sizeclass_to_slab_object_count(sizeclass_t sizeclass)
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

  inline static size_t large_size_to_chunk_size(size_t size)
  {
    return bits::next_pow2(size);
  }

  inline static size_t large_size_to_chunk_sizeclass(size_t size)
  {
    return bits::next_pow2_bits(size) - MIN_CHUNK_BITS;
  }
} // namespace snmalloc
