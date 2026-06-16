// SPDX-License-Identifier: MIT
//
// Heap profiler -- log2-spaced allocation-lifetime histogram (Phase 9.5).
//
// Records the lifetime (dealloc-time minus sample-time) of every sampled
// allocation that completes its lifecycle while the profiler is active.
// Bucket `i` covers lifetimes whose log2 nanosecond value falls in
// `[i, i+1)`, i.e. a lifetime of `n` nanoseconds bumps bucket
// `floor(log2(n))`.  Bucket 0 covers 1ns..2ns, bucket 31 covers
// ~2^31 ns ~ 2.1s and longer (saturating).
//
// This header is config-agnostic and depends only on `<atomic>` /
// `<cstdint>`, so it stays cheap to include and never re-enters the
// allocator on its own.  The hooking is driven by:
//
//   - `profile/sampled_alloc.h` -- adds an `alloc_ts_ns` field captured
//     at sample fire (see `sampler.h::record_alloc_slow`);
//   - `profile/record.h` -- in `clear_profile_slot`, the dealloc-time
//     path that recycles a sampled node computes the elapsed lifetime
//     and bumps the histogram bucket;
//   - `override/stats_export.cc` -- reads the buckets into
//     `FullAllocStats::lifetime_buckets_ns[]` when SNMALLOC_PROFILE is
//     defined.
//
// Concurrency: every bump is a relaxed `fetch_add` on the per-bucket
// counter.  No ordering relationship between buckets is assumed -- a
// snapshot reader may observe an inconsistent total across buckets,
// but that is acceptable for a histogram (the same property holds for
// e.g. the SampledList).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace snmalloc::profile
{
  /// Number of log2-spaced histogram buckets.  Must match
  /// `SNMALLOC_FULL_STATS_LIFETIME_BUCKETS` in
  /// `src/snmalloc/global/stats_export.h` so the C ABI struct can carry
  /// the histogram verbatim.
  inline constexpr size_t kLifetimeBuckets = 32;

  /**
   * Process-wide lifetime histogram.  One singleton per process; accessed
   * via `LifetimeHistogram::get()`.
   *
   * The instance lives in static storage so the histogram persists across
   * sampler lifecycles (e.g. profiling re-enabled after a pause keeps
   * earlier buckets intact).  When `SNMALLOC_PROFILE` is undefined this
   * type still compiles, but no caller bumps any bucket and the stats
   * exporter is also gated -- so consumers observe all-zero buckets.
   */
  class LifetimeHistogram
  {
  public:
    LifetimeHistogram() noexcept = default;
    LifetimeHistogram(const LifetimeHistogram&) = delete;
    LifetimeHistogram& operator=(const LifetimeHistogram&) = delete;

    /// Singleton accessor.  Constructed on first call; trivially-
    /// destructible array of `std::atomic<uint64_t>` so process-exit
    /// teardown order is not a concern.
    static LifetimeHistogram& get() noexcept
    {
      static LifetimeHistogram instance;
      return instance;
    }

    /**
     * Increment the bucket corresponding to a lifetime of `ns`
     * nanoseconds.  Bucket index = `floor(log2(ns))`, clamped to
     * `[0, kLifetimeBuckets - 1]`.  `ns == 0` is mapped to bucket 0
     * (any lifetime sub-nanosecond is best-counted in the shortest
     * bucket; in practice the clock resolution makes a true zero rare
     * but tolerable).
     */
    void record_lifetime_ns(uint64_t ns) noexcept
    {
      const size_t bucket = bucket_for(ns);
      buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
    }

    /// Read the current count for bucket `i` (`i < kLifetimeBuckets`).
    /// Relaxed load; the histogram does not preserve any cross-bucket
    /// ordering invariant.
    [[nodiscard]] uint64_t bucket(size_t i) const noexcept
    {
      return buckets_[i].load(std::memory_order_relaxed);
    }

    /**
     * Compute the histogram bucket for a lifetime of `ns` nanoseconds.
     * Exposed as a free helper so unit tests can verify bucketing
     * without going through the singleton.
     *
     *   bucket(0)  == 0   (sub-nanosecond / clock-skew fallback)
     *   bucket(1)  == 0
     *   bucket(2)  == 1
     *   bucket(3)  == 1
     *   bucket(4)  == 2
     *   ...
     *   bucket(2^k)            == k     for k in [0, 31]
     *   bucket(>= 2^31)        == 31    (saturating)
     */
    [[nodiscard]] static size_t bucket_for(uint64_t ns) noexcept
    {
      if (ns <= 1)
        return 0;
      // floor(log2(ns)) via 63 - clz.  We've already excluded ns == 0;
      // for ns == 1 the result is 0 which we return above.
#if defined(_MSC_VER)
      unsigned long index = 0;
      _BitScanReverse64(&index, ns);
      const size_t b = static_cast<size_t>(index);
#else
      const size_t b = static_cast<size_t>(63 - __builtin_clzll(ns));
#endif
      return b >= kLifetimeBuckets ? (kLifetimeBuckets - 1) : b;
    }

  private:
    std::atomic<uint64_t> buckets_[kLifetimeBuckets]{};
  };
} // namespace snmalloc::profile
