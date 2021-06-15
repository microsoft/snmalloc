#pragma once

#include "../pal/pal.h"
#include "allocconfig.h"

namespace snmalloc
{
  // Both usings should compile
  // We use size_t as it generates better code.
  using sizeclass_t = size_t;
  //  using sizeclass_t = uint8_t;
  using sizeclass_compress_t = uint8_t;

  constexpr static uintptr_t SIZECLASS_MASK = 0xFF;

  constexpr static uint16_t get_initial_offset(sizeclass_t sc, bool is_short);
  constexpr static uint16_t get_slab_capacity(sizeclass_t sc, bool is_short);

  constexpr static size_t sizeclass_to_size(sizeclass_t sizeclass);
  constexpr static uint16_t medium_slab_free(sizeclass_t sizeclass);
  static sizeclass_t size_to_sizeclass(size_t size);

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

  inline SNMALLOC_FAST_PATH static size_t round_size(size_t size)
  {
    if (size > sizeclass_to_size(NUM_SIZECLASSES - 1))
    {
      return bits::next_pow2(size);
    }
    if (size == 0)
    {
      size = 1;
    }
    return sizeclass_to_size(size_to_sizeclass(size));
  }

  // Uses table for reciprocal division, so provide forward reference.
  static bool is_multiple_of_sizeclass(sizeclass_t sc, size_t offset);

  /// Returns the alignment that this size naturally has, that is
  /// all allocations of size `size` will be aligned to the returned value.
  inline SNMALLOC_FAST_PATH static size_t natural_alignment(size_t size)
  {
    auto rsize = round_size(size);
    return bits::one_at_bit(bits::ctz(rsize));
  }
} // namespace snmalloc
