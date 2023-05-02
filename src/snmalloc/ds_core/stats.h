#include <atomic>
#include <cstddef>
#include "defines.h"

namespace snmalloc
{
  /**
   * Very basic statistic that tracks current and peak values.
   */
  class Stat
  {
  private:
    std::atomic<size_t> curr{0};
    std::atomic<size_t> peak{0};

  public:
    void increase(size_t amount)
    {
      size_t c = (curr += amount);
      size_t p = peak.load(std::memory_order_relaxed);
      while (c > p)
      {
        if (peak.compare_exchange_strong(p, c))
          break;
      }
    }

    void decrease(size_t amount)
    {
      size_t prev = curr.fetch_sub(amount);
      SNMALLOC_ASSERT(prev >= amount);
    }

    size_t get_curr()
    {
      return curr.load(std::memory_order_relaxed);
    }

    size_t get_peak()
    {
      return peak.load(std::memory_order_relaxed);
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
} // namespace snmalloc
