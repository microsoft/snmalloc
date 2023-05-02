#pragma once

#include <atomic>

namespace snmalloc
{
  /**
   * Used to measure memory usage.
   */
  template<typename ParentRange>
  class StatsRange
  {
    ParentRange parent{};

    static inline Stat usage{};

  public:
    static constexpr bool Aligned = ParentRange::Aligned;

    static constexpr bool ConcurrencySafe = ParentRange::ConcurrencySafe;

    constexpr StatsRange() = default;

    capptr::Chunk<void> alloc_range(size_t size)
    {
      auto result = parent.alloc_range(size);
      if (result != nullptr)
      {
        usage += size;
      }
      return result;
    }

    void dealloc_range(capptr::Chunk<void> base, size_t size)
    {
      usage -= size;
      parent.dealloc_range(base, size);
    }

    size_t get_current_usage()
    {
      return usage.get_curr();
    }

    size_t get_peak_usage()
    {
      return usage.get_peak();
    }
  };

  template<typename StatsR1, typename StatsR2>
  class StatsCombiner
  {
    StatsR1 r1{};
    StatsR2 r2{};

  public:
    size_t get_current_usage()
    {
      return r1.get_current_usage() + r2.get_current_usage();
    }

    size_t get_peak_usage()
    {
      return r1.get_peak_usage() + r2.get_peak_usage();
    }
  };
} // namespace snmalloc
