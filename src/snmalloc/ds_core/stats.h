#pragma once

#include "defines.h"
#include "snmalloc/stl/atomic.h"
#include "stddef.h"

namespace snmalloc
{
  /**
   * Very basic statistic that tracks current and peak values.
   */
  class Stat
  {
  private:
    stl::Atomic<size_t> curr{0};
    stl::Atomic<size_t> peak{0};

  public:
    void increase(size_t amount)
    {
      size_t old = curr.fetch_add(amount);
      size_t c = old + amount;
      size_t p = peak.load(stl::memory_order_relaxed);
      while (c > p)
      {
        if (peak.compare_exchange_strong(p, c))
          break;
      }
    }

    void decrease(size_t amount)
    {
      size_t prev = curr.fetch_sub(amount);
      SNMALLOC_ASSERT_MSG(
        prev >= amount, "prev = {}, amount = {}", prev, amount);
      UNUSED(prev);
    }

    size_t get_curr()
    {
      return curr.load(stl::memory_order_relaxed);
    }

    size_t get_peak()
    {
      return peak.load(stl::memory_order_relaxed);
    }

    void operator+=(size_t amount)
    {
      increase(amount);
    }

    void operator-=(size_t amount)
    {
      decrease(amount);
    }

    void operator++()
    {
      increase(1);
    }

    void operator--()
    {
      decrease(1);
    }
  };

  /**
   * Very basic statistic that can only grow.  Not thread-safe.
   */
  class MonotoneLocalStat
  {
    stl::Atomic<size_t> value{0};

  public:
    void operator++(int)
    {
      value.fetch_add(1, stl::memory_order_relaxed);
    }

    void operator+=(const MonotoneLocalStat& other)
    {
      auto v = other.value.load(stl::memory_order_relaxed);
      value.fetch_add(v, stl::memory_order_relaxed);
    }

    void operator+=(size_t v)
    {
      value.fetch_add(v, stl::memory_order_relaxed);
    }

    size_t operator*()
    {
      return value.load(stl::memory_order_relaxed);
    }
  };
} // namespace snmalloc
