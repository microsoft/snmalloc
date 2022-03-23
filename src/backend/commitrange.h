#pragma once

#include "../ds/ptrwrap.h"

namespace snmalloc
{
  template<
    SNMALLOC_CONCEPT(ConceptBackendRange_Alloc) ParentRange,
    SNMALLOC_CONCEPT(ConceptPAL) PAL>
  class CommitRange
  {
    typename ParentRange::State parent{};

  public:
    class State
    {
      CommitRange commit_range{};

    public:
      constexpr State() = default;

      CommitRange* operator->()
      {
        return &commit_range;
      }
    };

    using B = typename ParentRange::B;
    using KArg = typename ParentRange::KArg;

    static constexpr bool Aligned = ParentRange::Aligned;

    constexpr CommitRange() = default;

    CapPtr<void, B> alloc_range(KArg ka, size_t size)
    {
      auto range = parent->alloc_range(ka, size);
      if (range != nullptr)
        PAL::template notify_using<NoZero>(range.unsafe_ptr(), size);
      return range;
    }

    template<SNMALLOC_CONCEPT(ConceptBackendRange_Dealloc) _pr = ParentRange>
    void dealloc_range(KArg ka, CapPtr<void, B> base, size_t size)
    {
      static_assert(
        std::is_same<_pr, ParentRange>::value,
        "Don't set SFINAE template parameter!");
      PAL::notify_not_using(base.unsafe_ptr(), size);
      parent->dealloc_range(ka, base, size);
    }
  };
} // namespace snmalloc