#pragma once

#include "../ds/bits.h"
#include "../mem/sizeclass.h"

#include <cstdint>

#ifdef USE_SNMALLOC_STATS
#  include "../ds/csv.h"
#  include "sizeclass.h"

#  include <cstring>
#  include <iostream>
#endif

namespace snmalloc
{
  template<size_t N, size_t LARGE_N>
  struct AllocStats
  {
    struct CurrentMaxPair
    {
      size_t current = 0;
      size_t max = 0;
      size_t used = 0;

      void inc()
      {
        current++;
        used++;
        if (current > max)
          max++;
      }

      void dec()
      {
        SNMALLOC_ASSERT(current > 0);
        current--;
      }

      bool is_empty()
      {
        return current == 0;
      }

      bool is_unused()
      {
        return max == 0;
      }

      void add(CurrentMaxPair& that)
      {
        current += that.current;
        max += that.max;
        used += that.used;
      }
#ifdef USE_SNMALLOC_STATS
      void print(CSVStream& csv, size_t multiplier = 1)
      {
        csv << current * multiplier << max * multiplier << used * multiplier;
      }
#endif
    };

    struct Stats
    {
      CurrentMaxPair count;
      CurrentMaxPair slab_count;
      uint64_t time = Aal::tick();
      uint64_t ticks = 0;
      double online_average = 0;

      bool is_empty()
      {
        return count.is_empty();
      }

      void add(Stats& that)
      {
        count.add(that.count);
        slab_count.add(that.slab_count);
      }

      void addToRunningAverage()
      {
        uint64_t now = Aal::tick();

        if (slab_count.current != 0)
        {
          double occupancy = static_cast<double>(count.current) /
            static_cast<double>(slab_count.current);
          uint64_t duration = now - time;

          if (ticks == 0)
            online_average = occupancy;
          else
            online_average += ((occupancy - online_average) * duration) / ticks;

          ticks += duration;
        }

        time = now;
      }

#ifdef USE_SNMALLOC_STATS
      void
      print(CSVStream& csv, size_t multiplier = 1, size_t slab_multiplier = 1)
      {
        // Keep in sync with header lower down
        count.print(csv, multiplier);
        slab_count.print(csv, slab_multiplier);
        size_t average = static_cast<size_t>(online_average * multiplier);

        csv << average << (slab_multiplier - average) * slab_count.max
            << csv.endl;
      }
#endif
    };

#ifdef USE_SNMALLOC_STATS
    static constexpr size_t BUCKETS_BITS = 4;
    static constexpr size_t BUCKETS = 1 << BUCKETS_BITS;
    static constexpr size_t TOTAL_BUCKETS =
      bits::to_exp_mant_const<BUCKETS_BITS>(
        bits::one_at_bit(bits::ADDRESS_BITS - 1));

    Stats sizeclass[N];

    size_t large_pop_count[LARGE_N] = {0};
    size_t large_push_count[LARGE_N] = {0};

    size_t remote_freed = 0;
    size_t remote_posted = 0;
    size_t remote_received = 0;
    size_t superslab_push_count = 0;
    size_t superslab_pop_count = 0;
    size_t superslab_fresh_count = 0;
    size_t segment_count = 0;
    size_t bucketed_requests[TOTAL_BUCKETS] = {};
#endif

    void alloc_request(size_t size)
    {
      UNUSED(size);

#ifdef USE_SNMALLOC_STATS
      auto index = (size == 0) ? 0 : bits::to_exp_mant<BUCKETS_BITS>(size);
      SNMALLOC_ASSERT(index < TOTAL_BUCKETS);
      bucketed_requests[index]++;
#endif
    }

    bool is_empty()
    {
#ifdef USE_SNMALLOC_STATS
      for (size_t i = 0; i < N; i++)
      {
        if (!sizeclass[i].is_empty())
          return false;
      }

      for (size_t i = 0; i < LARGE_N; i++)
      {
        if (large_push_count[i] != large_pop_count[i])
          return false;
      }

      return (remote_freed == remote_posted);
#else
      return true;
#endif
    }

    void sizeclass_alloc(sizeclass_t sc)
    {
      UNUSED(sc);

#ifdef USE_SNMALLOC_STATS
      sizeclass[sc].addToRunningAverage();
      sizeclass[sc].count.inc();
#endif
    }

    void sizeclass_dealloc(sizeclass_t sc)
    {
      UNUSED(sc);

#ifdef USE_SNMALLOC_STATS
      sizeclass[sc].addToRunningAverage();
      sizeclass[sc].count.dec();
#endif
    }

    void large_alloc(size_t sc)
    {
      UNUSED(sc);

#ifdef USE_SNMALLOC_STATS
      SNMALLOC_ASSUME(sc < LARGE_N);
      large_pop_count[sc]++;
#endif
    }

    void sizeclass_alloc_slab(sizeclass_t sc)
    {
      UNUSED(sc);

#ifdef USE_SNMALLOC_STATS
      sizeclass[sc].addToRunningAverage();
      sizeclass[sc].slab_count.inc();
#endif
    }

    void sizeclass_dealloc_slab(sizeclass_t sc)
    {
      UNUSED(sc);

#ifdef USE_SNMALLOC_STATS
      sizeclass[sc].addToRunningAverage();
      sizeclass[sc].slab_count.dec();
#endif
    }

    void large_dealloc(size_t sc)
    {
      UNUSED(sc);

#ifdef USE_SNMALLOC_STATS
      large_push_count[sc]++;
#endif
    }

    void segment_create()
    {
#ifdef USE_SNMALLOC_STATS
      segment_count++;
#endif
    }

    void superslab_pop()
    {
#ifdef USE_SNMALLOC_STATS
      superslab_pop_count++;
#endif
    }

    void superslab_push()
    {
#ifdef USE_SNMALLOC_STATS
      superslab_push_count++;
#endif
    }

    void superslab_fresh()
    {
#ifdef USE_SNMALLOC_STATS
      superslab_fresh_count++;
#endif
    }

    void remote_free(sizeclass_t sc)
    {
      UNUSED(sc);

#ifdef USE_SNMALLOC_STATS
      remote_freed += sizeclass_to_size(sc);
#endif
    }

    void remote_post()
    {
#ifdef USE_SNMALLOC_STATS
      remote_posted = remote_freed;
#endif
    }

    void remote_receive(sizeclass_t sc)
    {
      UNUSED(sc);

#ifdef USE_SNMALLOC_STATS
      remote_received += sizeclass_to_size(sc);
#endif
    }

    void add(AllocStats<N, LARGE_N>& that)
    {
      UNUSED(that);

#ifdef USE_SNMALLOC_STATS
      for (size_t i = 0; i < N; i++)
        sizeclass[i].add(that.sizeclass[i]);

      for (size_t i = 0; i < LARGE_N; i++)
      {
        large_push_count[i] += that.large_push_count[i];
        large_pop_count[i] += that.large_pop_count[i];
      }

      for (size_t i = 0; i < TOTAL_BUCKETS; i++)
        bucketed_requests[i] += that.bucketed_requests[i];

      remote_freed += that.remote_freed;
      remote_posted += that.remote_posted;
      remote_received += that.remote_received;
      superslab_pop_count += that.superslab_pop_count;
      superslab_push_count += that.superslab_push_count;
      superslab_fresh_count += that.superslab_fresh_count;
      segment_count += that.segment_count;
#endif
    }

#ifdef USE_SNMALLOC_STATS
    template<class Alloc>
    void print(std::ostream& o, uint64_t dumpid = 0, uint64_t allocatorid = 0)
    {
      UNUSED(o);
      UNUSED(dumpid);
      UNUSED(allocatorid);

      CSVStream csv(&o);

      if (dumpid == 0)
      {
        // Output headers for initial dump
        // Keep in sync with data dump
        csv << "GlobalStats"
            << "DumpID"
            << "AllocatorID"
            << "Remote freed"
            << "Remote posted"
            << "Remote received"
            << "Superslab pop"
            << "Superslab push"
            << "Superslab fresh"
            << "Segments" << csv.endl;

        csv << "BucketedStats"
            << "DumpID"
            << "AllocatorID"
            << "Size group"
            << "Size"
            << "Current count"
            << "Max count"
            << "Total Allocs"
            << "Current Slab bytes"
            << "Max Slab bytes"
            << "Total slab allocs"
            << "Average Slab Usage"
            << "Average wasted space" << csv.endl;

        csv << "LargeBucketedStats"
            << "DumpID"
            << "AllocatorID"
            << "Size group"
            << "Size"
            << "Push count"
            << "Pop count" << csv.endl;

        csv << "AllocSizes"
            << "DumpID"
            << "AllocatorID"
            << "ClassID"
            << "Low size"
            << "High size"
            << "Count" << csv.endl;
      }

      for (sizeclass_t i = 0; i < N; i++)
      {
        if (sizeclass[i].count.is_unused())
          continue;

        sizeclass[i].addToRunningAverage();

        csv << "BucketedStats" << dumpid << allocatorid << i
            << sizeclass_to_size(i);

        sizeclass[i].print(csv, sizeclass_to_size(i));
      }

      for (uint8_t i = 0; i < LARGE_N; i++)
      {
        if ((large_push_count[i] == 0) && (large_pop_count[i] == 0))
          continue;

        csv << "LargeBucketedStats" << dumpid << allocatorid << (i + N)
            << large_sizeclass_to_size(i) << large_push_count[i]
            << large_pop_count[i] << csv.endl;
      }

      size_t low = 0;
      size_t high = 0;

      for (size_t i = 0; i < TOTAL_BUCKETS; i++)
      {
        low = high + 1;
        high = bits::from_exp_mant<BUCKETS_BITS>(i);

        if (bucketed_requests[i] == 0)
          continue;

        csv << "AllocSizes" << dumpid << allocatorid << i << low << high
            << bucketed_requests[i] << csv.endl;
      }

      csv << "GlobalStats" << dumpid << allocatorid << remote_freed
          << remote_posted << remote_received << superslab_pop_count
          << superslab_push_count << superslab_fresh_count << segment_count
          << csv.endl;
    }
#endif
  };
} // namespace snmalloc
