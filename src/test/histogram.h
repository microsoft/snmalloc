#pragma once

#ifdef USE_MEASURE
#  include "../ds/flaglock.h"

#  include <algorithm>
#  include <iomanip>
#  include <iostream>
#  define MEASURE_TIME_MARKERS(id, minbits, maxbits, markers) \
    static constexpr const char* const id##_time_markers[] = markers; \
    static histogram::Global<histogram::Histogram<uint64_t, minbits, maxbits>> \
      id##_time_global(#id, __FILE__, __LINE__, id##_time_markers); \
    static thread_local histogram::Histogram<uint64_t, minbits, maxbits> \
      id##_time_local(id##_time_global); \
    histogram::MeasureTime<histogram::Histogram<uint64_t, minbits, maxbits>> \
      id##_time(id##_time_local);

#  define MEASURE_TIME(id, minbits, maxbits) \
    MEASURE_TIME_MARKERS(id, minbits, maxbits, {nullptr})

#  define MARKERS(...) \
    { \
      __VA_ARGS__, nullptr \
    }

namespace histogram
{
  using namespace snmalloc;

  template<class H>
  class Global;

  template<
    class V,
    size_t LOW_BITS,
    size_t HIGH_BITS,
    size_t INTERMEDIATE_BITS = LOW_BITS>
  class Histogram
  {
  public:
    using This = Histogram<V, LOW_BITS, HIGH_BITS, INTERMEDIATE_BITS>;
    friend Global<This>;

    static_assert(LOW_BITS < HIGH_BITS, "LOW_BITS must be less than HIGH_BITS");

    static constexpr V LOW = (V)((size_t)1 << LOW_BITS);
    static constexpr V HIGH = (V)((size_t)1 << HIGH_BITS);
    static constexpr size_t BUCKETS =
      ((HIGH_BITS - LOW_BITS) << INTERMEDIATE_BITS) + 2;

  private:
    V high = (std::numeric_limits<V>::min)();
    size_t overflow;
    size_t count[BUCKETS];

    Global<This>* global;

  public:
    Histogram() : global(nullptr) {}
    Histogram(Global<This>& g) : global(&g) {}

    ~Histogram()
    {
      if (global != nullptr)
        global->add(*this);
    }

    void record(V value)
    {
      if (value > high)
        high = value;

      if (value >= HIGH)
      {
        overflow++;
      }
      else
      {
        auto i = get_index(value);
        SNMALLOC_ASSERT(i < BUCKETS);
        count[i]++;
      }
    }

    V get_high()
    {
      return high;
    }

    size_t get_overflow()
    {
      return overflow;
    }

    size_t get_buckets()
    {
      return BUCKETS;
    }

    size_t get_count(size_t index)
    {
      if (index >= BUCKETS)
        return 0;

      return count[index];
    }

    static std::pair<V, V> get_range(size_t index)
    {
      if (index >= BUCKETS)
        return std::make_pair(HIGH, HIGH);

      if (index == 0)
        return std::make_pair(0, get_value(index));

      return std::make_pair(get_value(index - 1) + 1, get_value(index));
    }

    void add(This& that)
    {
      high = (std::max)(high, that.high);
      overflow += that.overflow;

      for (size_t i = 0; i < BUCKETS; i++)
        count[i] += that.count[i];
    }

    void print(std::ostream& o)
    {
      o << "\tHigh: " << high << std::endl
        << "\tOverflow: " << overflow << std::endl;

      size_t grand_total = overflow;
      for (size_t i = 0; i < BUCKETS; i++)
        grand_total += count[i];

      size_t old_percentage = 0;
      size_t cumulative_total = 0;
      for (size_t i = 0; i < BUCKETS; i++)
      {
        auto r = get_range(i);

        cumulative_total += count[i];

        o << "\t" << std::setfill(' ') << std::setw(6) << std::get<0>(r) << ".."
          << std::setfill(' ') << std::setw(6) << std::get<1>(r) << ": "
          << std::setfill(' ') << std::setw(10) << count[i];

        auto percentage = (cumulative_total * 100 / grand_total);
        if (percentage != old_percentage)
        {
          old_percentage = percentage;
          o << std::setfill(' ') << std::setw(20)
            << (cumulative_total * 100 / grand_total) << "%";
        }

        o << std::endl;
      }
    }

    static size_t get_index(V value)
    {
      return bits::to_exp_mant<INTERMEDIATE_BITS, LOW_BITS - INTERMEDIATE_BITS>(
        value);
    }

    static V get_value(size_t index)
    {
      return bits::
        from_exp_mant<INTERMEDIATE_BITS, LOW_BITS - INTERMEDIATE_BITS>(index);
    }
  };

  template<class H>
  class Global
  {
  private:
    const char* name;
    const char* file;
    size_t line;
    const char* const* markers;

    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    H aggregate;

  public:
    Global(
      const char* name_,
      const char* file_,
      size_t line_,
      const char* const* markers)
    : name(name_), file(file_), line(line_), markers(markers)
    {}

    ~Global()
    {
      print();
    }

    void add(H& histogram)
    {
      FlagLock f(lock);
      aggregate.add(histogram);
    }

  private:
    void print()
    {
      std::cout << name;

      if (markers != nullptr)
      {
        std::cout << ": ";
        size_t i = 0;

        while (markers[i] != nullptr)
          std::cout << markers[i++] << " ";
      }

      std::cout << std::endl << file << ":" << line << std::endl;

      aggregate.print(std::cout);
    }
  };

  template<class H>
  class MeasureTime
  {
  private:
    H& histogram;
    uint64_t t;

  public:
    MeasureTime(H& histogram_) : histogram(histogram_)
    {
      t = bits::benchmark_time_start();
    }

    ~MeasureTime()
    {
      histogram.record(bits::benchmark_time_end() - t);
    }
  };
}

#else
#  define MEASURE_TIME(id, minbits, maxbits)
#  define MEASURE_TIME_MARKERS(id, minbits, maxbits, markers)
#endif
