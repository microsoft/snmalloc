#pragma once

#include "allocconfig.h"
#include "bits.h"

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

} // namespace snmalloc
