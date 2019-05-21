#pragma once

#include "allocconfig.h"

namespace snmalloc
{
  constexpr static uint16_t get_initial_bumpptr(uint8_t sc, bool is_short);
  constexpr static uint16_t get_initial_link(uint8_t sc, bool is_short);
  constexpr static size_t sizeclass_to_size(uint8_t sizeclass);
  constexpr static size_t sizeclass_to_cache_friendly_mask(uint8_t sizeclass);
  constexpr static size_t sizeclass_to_inverse_cache_friendly_mask(uint8_t sc);
  constexpr static uint16_t medium_slab_free(uint8_t sizeclass);

  static inline uint8_t size_to_sizeclass(size_t size)
  {
    // Don't use sizeclasses that are not a multiple of the alignment.
    // For example, 24 byte allocations can be
    // problematic for some data due to alignment issues.
    return static_cast<uint8_t>(
      bits::to_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(size));
  }

  constexpr static inline uint8_t size_to_sizeclass_const(size_t size)
  {
    // Don't use sizeclasses that are not a multiple of the alignment.
    // For example, 24 byte allocations can be
    // problematic for some data due to alignment issues.
    return static_cast<uint8_t>(
      bits::to_exp_mant_const<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(size));
  }

  constexpr static inline size_t large_sizeclass_to_size(uint8_t large_class)
  {
    return bits::one_at_bit(large_class + SUPERSLAB_BITS);
  }

  // Small classes range from [MIN, SLAB], i.e. inclusive.
  static constexpr size_t NUM_SMALL_CLASSES =
    size_to_sizeclass_const(bits::one_at_bit(SLAB_BITS)) + 1;

  static constexpr size_t NUM_SIZECLASSES =
    size_to_sizeclass_const(SUPERSLAB_SIZE);

  // Medium classes range from (SLAB, SUPERSLAB), i.e. non-inclusive.
  static constexpr size_t NUM_MEDIUM_CLASSES =
    NUM_SIZECLASSES - NUM_SMALL_CLASSES;

  // Large classes range from [SUPERSLAB, ADDRESS_SPACE).
  static constexpr size_t NUM_LARGE_CLASSES =
    bits::ADDRESS_BITS - SUPERSLAB_BITS;

  inline static size_t round_by_sizeclass(size_t rsize, size_t offset)
  {
    //    check_same<NUM_LARGE_CLASSES, Globals::num_large_classes>();
    // Must be called with a rounded size.
    assert(sizeclass_to_size(size_to_sizeclass(rsize)) == rsize);
    // Only works up to certain offsets, exhaustively tested upto
    // SUPERSLAB_SIZE.
    assert(offset <= SUPERSLAB_SIZE);

    size_t align = bits::ctz(rsize);
    size_t divider = rsize >> align;
    // Maximum of 24 bits for 16MiB super/medium slab
    if (INTERMEDIATE_BITS == 0 || divider == 1)
    {
      assert(divider == 1);
      return offset & ~(rsize - 1);
    }

    if constexpr (bits::is64() && INTERMEDIATE_BITS <= 2)
    {
      // Only works for 64 bit multiplication, as the following will overflow in
      // 32bit.
      // The code is using reciprocal division, with a shift of 26 bits, this
      // is considerably more bits than we need in the result.  If SUPERSLABS
      // get larger then we should review this code.
      static_assert(SUPERSLAB_BITS <= 24, "The following code assumes 24 bits");
      static constexpr size_t shift = 26;
      size_t back_shift = shift + align;
      static constexpr size_t mul_shift = 1ULL << shift;
      static constexpr uint32_t constants[8] = {0,
                                                mul_shift,
                                                0,
                                                (mul_shift / 3) + 1,
                                                0,
                                                (mul_shift / 5) + 1,
                                                0,
                                                (mul_shift / 7) + 1};
      return ((constants[divider] * offset) >> back_shift) * rsize;
    }
    else
      // Use 32-bit division as considerably faster than 64-bit, and
      // everything fits into 32bits here.
      return static_cast<uint32_t>(offset / rsize) * rsize;
  }

  inline static bool is_multiple_of_sizeclass(size_t rsize, size_t offset)
  {
    // Must be called with a rounded size.
    assert(sizeclass_to_size(size_to_sizeclass(rsize)) == rsize);
    // Only works up to certain offsets, exhaustively tested upto
    // SUPERSLAB_SIZE.
    assert(offset <= SUPERSLAB_SIZE);

    size_t align = bits::ctz(rsize);
    size_t divider = rsize >> align;
    // Maximum of 24 bits for 16MiB super/medium slab
    if (INTERMEDIATE_BITS == 0 || divider == 1)
    {
      assert(divider == 1);
      return (offset & (rsize - 1)) == 0;
    }

    if constexpr (bits::is64() && INTERMEDIATE_BITS <= 2)
    {
      // Only works for 64 bit multiplication, as the following will overflow in
      // 32bit.
      // The code is using reciprocal division, with a shift of 26 bits, this
      // is considerably more bits than we need in the result.  If SUPERSLABS
      // get larger then we should review this code.
      static_assert(SUPERSLAB_BITS <= 24, "The following code assumes 24 bits");
      static constexpr size_t shift = 31;
      static constexpr size_t mul_shift = 1ULL << shift;
      static constexpr uint32_t constants[8] = {0,
                                                mul_shift,
                                                0,
                                                (mul_shift / 3) + 1,
                                                0,
                                                (mul_shift / 5) + 1,
                                                0,
                                                (mul_shift / 7) + 1};

      // There is a long chain of zeros after the backshift
      // However, not all zero so just check a range.
      // This is exhaustively tested for the current use case
      return (((constants[divider] * offset)) &
              (((1ULL << (align + 3)) - 1) << (shift - 3))) == 0;
    }
    else
      // Use 32-bit division as considerably faster than 64-bit, and
      // everything fits into 32bits here.
      return static_cast<uint32_t>(offset % rsize) == 0;
  }

#ifdef CACHE_FRIENDLY_OFFSET
  inline static void* remove_cache_friendly_offset(void* p, uint8_t sizeclass)
  {
    size_t mask = sizeclass_to_inverse_cache_friendly_mask(sizeclass);
    return p = (void*)((uintptr_t)p & mask);
  }

  inline static uint16_t
  remove_cache_friendly_offset(uint16_t relative, uint8_t sizeclass)
  {
    size_t mask = sizeclass_to_inverse_cache_friendly_mask(sizeclass);
    return relative & mask;
  }
#else
  inline static void* remove_cache_friendly_offset(void* p, uint8_t sizeclass)
  {
    UNUSED(sizeclass);
    return p;
  }
  inline static uint16_t
  remove_cache_friendly_offset(uint16_t relative, uint8_t sizeclass)
  {
    UNUSED(sizeclass);
    return relative;
  }
#endif
} // namespace snmalloc
