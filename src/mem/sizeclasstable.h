#pragma once

#include "../ds/helpers.h"
#include "superslab.h"

namespace snmalloc
{
  constexpr size_t PTR_BITS = bits::next_pow2_bits_const(sizeof(void*));

  constexpr static SNMALLOC_PURE size_t sizeclass_lookup_index(const size_t s)
  {
    // We subtract and shirt to reduce the size of the table, i.e. we don't have
    // to store a value for every size class.
    // We could shift by MIN_ALLOC_BITS, as this would give us the most
    // compressed table, but by shifting by PTR_BITS the code-gen is better
    // as the most important path using this subsequently shifts left by
    // PTR_BITS, hence they can be fused into a single mask.
    return (s - 1) >> PTR_BITS;
  }

  constexpr static size_t sizeclass_lookup_size =
    sizeclass_lookup_index(SLAB_SIZE + 1);

  struct SizeClassTable
  {
    sizeclass_t sizeclass_lookup[sizeclass_lookup_size] = {{}};
    ModArray<NUM_SIZECLASSES, size_t> size;
    ModArray<NUM_SIZECLASSES, size_t> cache_friendly_mask;
    ModArray<NUM_SIZECLASSES, size_t> inverse_cache_friendly_mask;
    ModArray<NUM_SMALL_CLASSES, uint16_t> initial_offset_ptr;
    ModArray<NUM_SMALL_CLASSES, uint16_t> short_initial_offset_ptr;
    ModArray<NUM_MEDIUM_CLASSES, uint16_t> medium_slab_slots;
    // Table of constants for reciprocal division for each sizeclass.
    ModArray<NUM_SIZECLASSES, size_t> div_mult;
    // Table of constants for reciprocal modulus for each sizeclass.
    ModArray<NUM_SIZECLASSES, size_t> mod_mult;

    constexpr SizeClassTable()
    : size(),
      cache_friendly_mask(),
      inverse_cache_friendly_mask(),
      initial_offset_ptr(),
      short_initial_offset_ptr(),
      medium_slab_slots(),
      div_mult(),
      mod_mult()
    {
      size_t curr = 1;
      for (sizeclass_t sizeclass = 0; sizeclass < NUM_SIZECLASSES; sizeclass++)
      {
        size[sizeclass] =
          bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(sizeclass);

        div_mult[sizeclass] =
          (bits::one_at_bit(bits::BITS - SUPERSLAB_BITS) /
           (size[sizeclass] / MIN_ALLOC_SIZE));
        if (!bits::is_pow2(size[sizeclass]))
          div_mult[sizeclass]++;

        mod_mult[sizeclass] =
          (bits::one_at_bit(bits::BITS - 1) / size[sizeclass]);
        if (!bits::is_pow2(size[sizeclass]))
          mod_mult[sizeclass]++;

        if (sizeclass < NUM_SMALL_CLASSES)
        {
          for (; curr <= size[sizeclass]; curr += 1 << PTR_BITS)
          {
            sizeclass_lookup[sizeclass_lookup_index(curr)] = sizeclass;
          }
        }

        size_t alignment = bits::min(
          bits::one_at_bit(bits::ctz_const(size[sizeclass])), OS_PAGE_SIZE);
        cache_friendly_mask[sizeclass] = (alignment - 1);
        inverse_cache_friendly_mask[sizeclass] = ~(alignment - 1);
      }

      size_t header_size = sizeof(Superslab);
      size_t short_slab_size = SLAB_SIZE - header_size;

      for (sizeclass_t i = 0; i < NUM_SMALL_CLASSES; i++)
      {
        // We align to the end of the block to remove special cases for the
        // short block. Calculate remainders
        size_t short_correction = short_slab_size % size[i];
        size_t correction = SLAB_SIZE % size[i];

        // First element in the block is the link
        initial_offset_ptr[i] = static_cast<uint16_t>(correction);
        short_initial_offset_ptr[i] =
          static_cast<uint16_t>(header_size + short_correction);
      }

      for (sizeclass_t i = NUM_SMALL_CLASSES; i < NUM_SIZECLASSES; i++)
      {
        medium_slab_slots[i - NUM_SMALL_CLASSES] = static_cast<uint16_t>(
          (SUPERSLAB_SIZE - Mediumslab::header_size()) / size[i]);
      }
    }
  };

  static constexpr SizeClassTable sizeclass_metadata = SizeClassTable();

  static inline constexpr uint16_t
  get_initial_offset(sizeclass_t sc, bool is_short)
  {
    if (is_short)
      return sizeclass_metadata.short_initial_offset_ptr[sc];

    return sizeclass_metadata.initial_offset_ptr[sc];
  }

  constexpr static inline size_t sizeclass_to_size(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.size[sizeclass];
  }

  constexpr static inline size_t
  sizeclass_to_cache_friendly_mask(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.cache_friendly_mask[sizeclass];
  }

  constexpr static SNMALLOC_FAST_PATH size_t
  sizeclass_to_inverse_cache_friendly_mask(sizeclass_t sizeclass)
  {
    return sizeclass_metadata.inverse_cache_friendly_mask[sizeclass];
  }

  static inline sizeclass_t size_to_sizeclass(size_t size)
  {
    if ((size - 1) <= (SLAB_SIZE - 1))
    {
      auto index = sizeclass_lookup_index(size);
      SNMALLOC_ASSUME(index <= sizeclass_lookup_index(SLAB_SIZE));
      return sizeclass_metadata.sizeclass_lookup[index];
    }

    // Don't use sizeclasses that are not a multiple of the alignment.
    // For example, 24 byte allocations can be
    // problematic for some data due to alignment issues.
    return static_cast<sizeclass_t>(
      bits::to_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(size));
  }

  constexpr static inline uint16_t medium_slab_free(sizeclass_t sizeclass)
  {
    return sizeclass_metadata
      .medium_slab_slots[(sizeclass - NUM_SMALL_CLASSES)];
  }

  inline static size_t round_by_sizeclass(sizeclass_t sc, size_t offset)
  {
    // Only works up to certain offsets, exhaustively tested upto
    // SUPERSLAB_SIZE.
    SNMALLOC_ASSERT(offset <= SUPERSLAB_SIZE);

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
      static_assert(
        SUPERSLAB_BITS <= 24, "The following code assumes max of 24 bits");

      return (((offset >> MIN_ALLOC_BITS) * sizeclass_metadata.div_mult[sc]) >>
              (bits::BITS - SUPERSLAB_BITS)) *
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
    SNMALLOC_ASSERT(offset <= SUPERSLAB_SIZE);

    if constexpr (bits::is64())
    {
      // Only works for 64 bit multiplication, as the following will overflow in
      // 32bit.
      // The code is using reciprocal division.  If SUPERSLABS
      // get larger then we should review this code.  The modulus code
      // has fewer restrictions than division, as it only requires the
      // square of the offset to be representable.
      static_assert(
        SUPERSLAB_BITS <= 24, "The following code assumes max of 24 bits");
      static constexpr size_t MASK = (bits::one_at_bit(bits::BITS - 1) - 1) ^
        (bits::one_at_bit(bits::BITS - 1 - SUPERSLAB_BITS) - 1);

      return ((offset * sizeclass_metadata.mod_mult[sc]) & MASK) == 0;
    }
    else
      // Use 32-bit division as considerably faster than 64-bit, and
      // everything fits into 32bits here.
      return static_cast<uint32_t>(offset % sizeclass_to_size(sc)) == 0;
  }

} // namespace snmalloc
