#pragma once
#include "../ds/address.h"
#include "../pal/pal.h"

namespace snmalloc
{
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL>
  class PalRange
  {
  public:
    class State
    {
    public:
      PalRange* operator->()
      {
        // There is no state required for the PalRange
        // using a global just to satisfy the typing.
        static PalRange range{};
        return &range;
      }

      constexpr State() = default;
    };

    using B = capptr::bounds::Chunk;

    static constexpr bool Aligned = pal_supports<AlignedAllocation, PAL>;

    constexpr PalRange() = default;

    CapPtr<void, B> alloc_range(size_t size)
    {
      if (bits::next_pow2_bits(size) >= bits::BITS - 1)
      {
        return nullptr;
      }

      if constexpr (pal_supports<AlignedAllocation, PAL>)
      {
        SNMALLOC_ASSERT(size >= PAL::minimum_alloc_size);
        auto result =
          CapPtr<void, B>(PAL::template reserve_aligned<false>(size));

#ifdef SNMALLOC_TRACING
        message<1024>("Pal range alloc: {} ({})", result.unsafe_ptr(), size);
#endif
        return result;
      }
      else
      {
        auto result = CapPtr<void, B>(PAL::reserve(size));

#ifdef SNMALLOC_TRACING
        message<1024>("Pal range alloc: {} ({})", result.unsafe_ptr(), size);
#endif

        return result;
      }
    }
  };
} // namespace snmalloc
