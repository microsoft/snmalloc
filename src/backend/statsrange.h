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

    static constexpr bool Aligned = ParentRange::Aligned;

    constexpr StatsRange() = default;

    capptr::Chunk<void> alloc_range(size_t size)
    {
      auto result = parent->alloc_range(size);
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

    void dealloc_range(capptr::Chunk<void> base, size_t size)
    {
      current_usage -= size;
      parent->dealloc_range(base, size);
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