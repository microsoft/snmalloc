#pragma once

#include "../ds/ptrwrap.h"

namespace snmalloc
{
  template<typename T>
  static T& IndirectRange_default_kparent(T* x)
  {
    return *x;
  }

  template<
    typename ParentRange,
    typename KArg_ = typename ParentRange::State*,
    typename ParentRange::State& (*KParent)(KArg_) =
      IndirectRange_default_kparent,
    typename ParentRange::KArg (*KPArg)(KArg_) = [](KArg_) { return nullptr; }>
  class IndirectRange
  {
  public:
    class State
    {
      IndirectRange this_range{};

    public:
      IndirectRange* operator->()
      {
        return &this_range;
      }

      constexpr State() = default;
    };

    using KArg = KArg_;

    static constexpr bool Aligned = ParentRange::Aligned;

    constexpr IndirectRange() = default;

    using B = capptr::bounds::Chunk;

    CapPtr<void, B> alloc_range(KArg ka, size_t size)
    {
      return KParent(ka)->alloc_range(KPArg(ka), size);
    }

    // template<SNMALLOC_CONCEPT(ConceptBackendRange_Dealloc) _pr = ParentRange>
    void dealloc_range(KArg ka, CapPtr<void, B> base, size_t size)
    {
      /*
            static_assert(
              std::is_same<_pr, ParentRange>::value,
              "Don't set SFINAE template parameter!");
      */
      KParent(ka)->dealloc_range(KPArg(ka), base, size);
    }
  };

  template<typename ParentRange, typename KArg_>
  class DropArgRange
  {
    static_assert(std::is_same_v<std::nullptr_t, typename ParentRange::KArg>);

    typename ParentRange::State parent{};

  public:
    class State
    {
      DropArgRange noarg_range{};

    public:
      constexpr DropArgRange* operator->()
      {
        return &noarg_range;
      }

      constexpr State() = default;
    };

    using KArg = KArg_;

    static constexpr bool Aligned = ParentRange::Aligned;

    constexpr DropArgRange() = default;

    using B = capptr::bounds::Chunk;

    CapPtr<void, B> alloc_range(KArg, size_t size)
    {
      return parent->alloc_range(nullptr, size);
    }

    // template<SNMALLOC_CONCEPT(ConceptBackendRange_Dealloc) _pr = ParentRange>
    void dealloc_range(KArg, CapPtr<void, B> base, size_t size)
    {
      /*
            static_assert(
              std::is_same<_pr, ParentRange>::value,
              "Don't set SFINAE template parameter!");
      */
      parent->dealloc_range(nullptr, base, size);
    }
  };
} // namespace snmalloc
