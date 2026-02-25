#include "allocconfig.h"
#include "bits.h"

namespace snmalloc
{
  using smallsizeclass_t = size_t;

  static constexpr smallsizeclass_t size_to_sizeclass_const(size_t size)
  {
    // Don't use sizeclasses that are not a multiple of the alignment.
    // For example, 24 byte allocations can be
    // problematic for some data due to alignment issues.
    auto sc = static_cast<smallsizeclass_t>(
      bits::to_exp_mant_const<INTERMEDIATE_BITS, MIN_ALLOC_STEP_BITS>(size));

    SNMALLOC_ASSERT(sc == static_cast<uint8_t>(sc));

    return sc;
  }

  constexpr size_t NUM_SMALL_SIZECLASSES =
    size_to_sizeclass_const(MAX_SMALL_SIZECLASS_SIZE) + 1;

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
    return (size - 1) < sizeclass_to_size_const(NUM_SMALL_SIZECLASSES - 1);
  }
} // namespace snmalloc
