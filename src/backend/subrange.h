#pragma once
#include "../mem/entropy.h"

namespace snmalloc
{
  /**
   * Creates an area inside a large allocation that is larger by
   * 2^RATIO_BITS.  Will not return a the block at the start or
   * the end of the large allocation.
   */
  template<
    SNMALLOC_CONCEPT(ConceptBackendRange_Alloc) ParentRange,
    SNMALLOC_CONCEPT(ConceptPAL) PAL,
    size_t RATIO_BITS>
  class SubRange
  {
    typename ParentRange::State parent{};

  public:
    class State
    {
      SubRange sub_range{};

    public:
      constexpr State() = default;

      SubRange* operator->()
      {
        return &sub_range;
      }
    };

    constexpr SubRange() = default;

    using B = typename ParentRange::B;
    using KArg = typename ParentRange::KArg;

    static constexpr bool Aligned = ParentRange::Aligned;

    CapPtr<void, B> alloc_range(KArg ka, size_t sub_size)
    {
      SNMALLOC_ASSERT(bits::is_pow2(sub_size));

      auto full_size = sub_size << RATIO_BITS;
      auto overblock = parent->alloc_range(ka, full_size);
      if (overblock == nullptr)
        return nullptr;

      size_t offset_mask = full_size - sub_size;
      // Don't use first or last block in the larger reservation
      // Loop required to get uniform distribution.
      size_t offset;
      do
      {
        offset = get_entropy64<PAL>() & offset_mask;
      } while ((offset == 0) || (offset == offset_mask));

      return pointer_offset(overblock, offset);
    }
  };
} // namespace snmalloc
