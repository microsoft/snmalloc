#pragma once

#include <atomic>

namespace snmalloc
{
  /**
   * Used to measure memory usage.
   */
  template<SNMALLOC_CONCEPT(ConceptBackendRange_Alloc) ParentRange>
  class StatsRange
  {
    typename ParentRange::State parent{};

    static inline std::atomic<size_t> current_usage{};
    static inline std::atomic<size_t> peak_usage{};

  public:
    class State
    {
      StatsRange stats_range{};

    public:
      constexpr StatsRange* operator->()
      {
        return &stats_range;
      }

      constexpr State() = default;
    };

    using B = typename ParentRange::B;
    using KArg = typename ParentRange::KArg;

    static constexpr bool Aligned = ParentRange::Aligned;

    constexpr StatsRange() = default;

    CapPtr<void, B> alloc_range(KArg ka, size_t size)
    {
      auto result = parent->alloc_range(ka, size);
      if (result != nullptr)
      {
        auto prev = current_usage.fetch_add(size);
        auto curr = peak_usage.load();
        while (curr < prev + size)
        {
          if (peak_usage.compare_exchange_weak(curr, prev + size))
            break;
        }
      }
      return result;
    }

    template<SNMALLOC_CONCEPT(ConceptBackendRange_Dealloc) _pr = ParentRange>
    void dealloc_range(KArg ka, CapPtr<void, B> base, size_t size)
    {
      static_assert(
        std::is_same<_pr, ParentRange>::value,
        "Don't set SFINAE template parameter!");
      current_usage -= size;
      parent->dealloc_range(ka, base, size);
    }

    size_t get_current_usage()
    {
      return current_usage.load();
    }

    size_t get_peak_usage()
    {
      return peak_usage.load();
    }
  };
} // namespace snmalloc
