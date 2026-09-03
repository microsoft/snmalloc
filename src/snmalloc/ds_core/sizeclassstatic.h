#pragma once

#include "sizeclassconfig.h"

namespace snmalloc
{
  /**
   * A wrapper type for small sizeclass indices.
   *
   * Implicitly converts TO size_t (for array indexing, comparisons, etc.)
   * but does NOT implicitly convert FROM size_t — construction must be
   * explicit via smallsizeclass_t(value).
   */
  struct smallsizeclass_t
  {
    size_t raw{0};

    constexpr smallsizeclass_t() = default;

    explicit constexpr smallsizeclass_t(size_t v) : raw(v) {}

    /// Implicit conversion to size_t.
    constexpr operator size_t() const
    {
      return raw;
    }

    /// Pre-increment.
    constexpr smallsizeclass_t& operator++()
    {
      ++raw;
      return *this;
    }

    /// Post-increment.
    constexpr smallsizeclass_t operator++(int)
    {
      auto tmp = *this;
      ++raw;
      return tmp;
    }
  };

  static constexpr smallsizeclass_t size_to_sizeclass_const(size_t size)
  {
    // Don't use sizeclasses that are not a multiple of the alignment.
    // For example, 24 byte allocations can be
    // problematic for some data due to alignment issues.
    return smallsizeclass_t(
      bits::to_exp_mant_const<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(size));
  }

  constexpr size_t NUM_SMALL_SIZECLASSES =
    size_t(size_to_sizeclass_const(MAX_SMALL_SIZECLASS_SIZE)) + 1;

  static_assert(
    NUM_SMALL_SIZECLASSES <= 256,
    "NUM_SMALL_SIZECLASSES must fit in the compressed small sizeclass "
    "representation");

  static constexpr size_t sizeclass_to_size_const(smallsizeclass_t sc)
  {
    return bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(sc);
  }

  /**
   * @brief Returns true if the size is a small sizeclass. Note that
   * 0 is not considered a small sizeclass.
   */
  constexpr bool is_small_sizeclass(size_t size)
  {
    // Perform the - 1 on size, so that zero wraps around and ends up on
    // slow path.
    return (size - 1) <
      sizeclass_to_size_const(smallsizeclass_t(NUM_SMALL_SIZECLASSES - 1));
  }

  /**
   * @brief Round `size` up so the resulting allocation can satisfy
   * the requested `alignment`. `alignment` must be a non-zero power
   * of two.
   *
   * Lives in sizeclassstatic.h (not sizeclasstable.h) so it is
   * available to compile-time-only consumers — notably the test
   * library header — without pulling in the full runtime sizeclass
   * machinery.
   */
  constexpr SNMALLOC_FAST_PATH size_t
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
