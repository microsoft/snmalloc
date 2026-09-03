#pragma once

#include "bits.h"

namespace snmalloc
{
  // 0 intermediate bits results in power of 2 small allocs. 1 intermediate
  // bit gives additional sizeclasses at the midpoint between each power of 2.
  // 2 intermediate bits gives 3 intermediate sizeclasses, etc.
  static constexpr size_t INTERMEDIATE_BITS =
#ifdef USE_INTERMEDIATE_BITS
    USE_INTERMEDIATE_BITS
#else
    2
#endif
    ;

  // The remaining values are derived, not configurable.
  static constexpr size_t POINTER_BITS =
    bits::next_pow2_bits_const(sizeof(uintptr_t));

  // Used to isolate values on cache lines to prevent false sharing.
  static constexpr size_t CACHELINE_SIZE = 64;

  /// The "machine epsilon" for the small sizeclass machinery.
  static constexpr size_t MIN_ALLOC_STEP_SIZE =
#if defined(SNMALLOC_MIN_ALLOC_STEP_SIZE)
    SNMALLOC_MIN_ALLOC_STEP_SIZE;
#else
    2 * sizeof(void*);
#endif

  /// Derived from MIN_ALLOC_STEP_SIZE
  static constexpr size_t MIN_ALLOC_STEP_BITS =
    bits::ctz_const(MIN_ALLOC_STEP_SIZE);
  static_assert(bits::is_pow2(MIN_ALLOC_STEP_SIZE));

  /**
   * Minimum allocation size is space for two pointers.  If the small sizeclass
   * machinery permits smaller values (that is, if MIN_ALLOC_STEP_SIZE is
   * smaller than MIN_ALLOC_SIZE), which may be useful if MIN_ALLOC_SIZE must
   * be large or not a power of two, those smaller size classes will be unused.
   */
  static constexpr size_t MIN_ALLOC_SIZE =
#if defined(SNMALLOC_MIN_ALLOC_SIZE)
    SNMALLOC_MIN_ALLOC_SIZE;
#else
    2 * sizeof(void*);
#endif

  // Minimum slab size.
#if defined(SNMALLOC_QEMU_WORKAROUND) && defined(SNMALLOC_VA_BITS_64)
  static constexpr size_t MIN_CHUNK_BITS = static_cast<size_t>(17);
#else
  static constexpr size_t MIN_CHUNK_BITS = static_cast<size_t>(14);
#endif
  static constexpr size_t MIN_CHUNK_SIZE = bits::one_at_bit(MIN_CHUNK_BITS);

  // Maximum size of an object that uses sizeclasses.
#if defined(SNMALLOC_QEMU_WORKAROUND) && defined(SNMALLOC_VA_BITS_64)
  static constexpr size_t MAX_SMALL_SIZECLASS_BITS = 19;
#else
  static constexpr size_t MAX_SMALL_SIZECLASS_BITS = 16;
#endif
  static constexpr size_t MAX_SMALL_SIZECLASS_SIZE =
    bits::one_at_bit(MAX_SMALL_SIZECLASS_BITS);

  static_assert(
    MAX_SMALL_SIZECLASS_SIZE >= MIN_CHUNK_SIZE,
    "Large sizes need to be representable by as a multiple of MIN_CHUNK_SIZE");

  static_assert(
    INTERMEDIATE_BITS < MIN_ALLOC_STEP_BITS,
    "INTERMEDIATE_BITS must be less than MIN_ALLOC_BITS");
  static_assert(
    MIN_ALLOC_SIZE >= (sizeof(void*) * 2),
    "MIN_ALLOC_SIZE must be sufficient for two pointers");
  static_assert(
    1 << (INTERMEDIATE_BITS + MIN_ALLOC_STEP_BITS) >=
      bits::next_pow2_const(MIN_ALLOC_SIZE),
    "Entire sizeclass exponent is below MIN_ALLOC_SIZE; adjust STEP_SIZE");
  static_assert(
    MIN_ALLOC_SIZE >= MIN_ALLOC_STEP_SIZE,
    "Minimum alloc sizes below minimum step size; raise MIN_ALLOC_SIZE");
} // namespace snmalloc
