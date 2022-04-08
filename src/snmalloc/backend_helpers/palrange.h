#pragma once
#include "../pal/pal.h"

namespace snmalloc
{
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL>
  class PalRange
  {
  public:
    static constexpr bool Aligned = pal_supports<AlignedAllocation, PAL>;

    // Note we have always assumed the Pals to provide a concurrency safe
    // API.  If in the future this changes, then this would
    // need to be changed.
    static constexpr bool ConcurrencySafe = true;

    constexpr PalRange() = default;

    capptr::Chunk<void> alloc_range(size_t size)
    {
      if (bits::next_pow2_bits(size) >= bits::BITS - 1)
      {
        return nullptr;
      }

      if constexpr (pal_supports<AlignedAllocation, PAL>)
      {
        SNMALLOC_ASSERT(size >= PAL::minimum_alloc_size);
        auto result =
          capptr::Chunk<void>(PAL::template reserve_aligned<false>(size));

#ifdef SNMALLOC_TRACING
        message<1024>("Pal range alloc: {} ({})", result.unsafe_ptr(), size);
#endif
        return result;
      }
      else
      {
        auto result = capptr::Chunk<void>(PAL::reserve(size));

#ifdef SNMALLOC_TRACING
        message<1024>("Pal range alloc: {} ({})", result.unsafe_ptr(), size);
#endif

        return result;
      }
    }
  };
} // namespace snmalloc
