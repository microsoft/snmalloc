#pragma once

#include "empty_range.h"
#include "range_helpers.h"
#include "snmalloc/stl/atomic.h"

namespace snmalloc
{
  /**
   * Used to measure memory usage.
   */
  struct StatsRange
  {
    template<typename ParentRange = EmptyRange<>>
    class Type : public ContainsParent<ParentRange>
    {
      using ContainsParent<ParentRange>::parent;

      static inline Stat usage{};

    public:
      static constexpr bool Aligned = ParentRange::Aligned;

      static constexpr bool ConcurrencySafe = ParentRange::ConcurrencySafe;

      using ChunkBounds = typename ParentRange::ChunkBounds;

      constexpr Type() = default;

      CapPtr<void, ChunkBounds> alloc_range(size_t size)
      {
        auto r = parent.alloc_range(size);
        if (r != nullptr)
          usage += size;
        return r;
      }

      void dealloc_range(CapPtr<void, ChunkBounds> base, size_t size)
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
