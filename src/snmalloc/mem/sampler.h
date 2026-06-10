#pragma once

#ifdef SNMALLOC_PROFILE

#  include "snmalloc/ds_core/defines.h"
#  include "snmalloc/stl/atomic.h"

#  include <climits>
#  include <cmath>
#  include <cstddef>
#  include <cstdint>

namespace snmalloc
{
  /**
   * Sampling interval in bytes. record() fires on average once per interval.
   * Set to 0 to disable sampling entirely.
   */
  inline stl::Atomic<size_t> g_sample_interval{512 * 1024};

  /**
   * Per-thread Poisson sampler for heap profiling.
   *
   * Models the allocation stream as a byte sequence with each byte
   * independently marked with probability 1/interval. An allocation
   * is sampled iff it contains at least one marked byte, giving:
   *
   *   P(sample) = 1 - e^(-size/interval)
   *
   * Fast path: one subtraction and branch per allocation.
   * Slow path: geometric sample + PRNG step, taken ~once per interval bytes.
   *
   * Not thread-safe — one Sampler per allocator (which is per-thread).
   */
  class Sampler
  {
    // Counts down bytes until next sample. Goes negative when a sample fires.
    ssize_t bytes_until_sample_{0};

    // xorshift64 PRNG. Zero means uninitialised.
    uint64_t rng_{0};

  public:
    /**
     * Account for an allocation of `size` bytes.
     * Returns 0 if not sampled, or a positive weight if this allocation
     * should be recorded. The weight is an estimate of how many bytes
     * this sample statistically represents.
     */
    SNMALLOC_FAST_PATH size_t record(size_t size) noexcept
    {
      bytes_until_sample_ -= static_cast<ssize_t>(size);
      if (SNMALLOC_LIKELY(bytes_until_sample_ > 0))
        return 0;
      return record_slow(size);
    }

  private:
    SNMALLOC_SLOW_PATH size_t record_slow(size_t size) noexcept
    {
      size_t interval = g_sample_interval.load(stl::memory_order_relaxed);

      if (interval == 0)
      {
        // Sampling disabled — park counter far in the future.
        bytes_until_sample_ = SSIZE_MAX / 2;
        return 0;
      }

      if (SNMALLOC_UNLIKELY(rng_ == 0))
      {
        // First use: seed the PRNG from the allocator's address, then
        // pick an initial sample point and re-apply the current allocation.
        rng_ = (reinterpret_cast<uintptr_t>(this) ^ 0xdeadbeef01234567ULL) | 1;
        bytes_until_sample_ = geometric(interval);
        bytes_until_sample_ -= static_cast<ssize_t>(size);
        if (bytes_until_sample_ > 0)
          return 0;
        // Fell through: this allocation also triggers a sample.
      }

      // Weight: bytes this sample represents ≈ interval.
      // (Exact: interval minus the overshoot, but interval is the
      //  unbiased estimator and sufficient for Phase 2.)
      size_t weight = interval;
      bytes_until_sample_ = geometric(interval);
      return weight;
    }

    // Geometric random variable with the given mean, via inverse CDF:
    //   gap = -log(U) * mean,  U ~ Uniform(0, 1]
    ssize_t geometric(size_t mean) noexcept
    {
      // xorshift64
      rng_ ^= rng_ << 13;
      rng_ ^= rng_ >> 7;
      rng_ ^= rng_ << 17;

      // Map top 53 bits to (0, 1].
      double u = static_cast<double>(rng_ >> 11) / double(uint64_t(1) << 53);
      if (SNMALLOC_UNLIKELY(u == 0.0))
        u = 1e-300;

      return static_cast<ssize_t>(-std::log(u) * static_cast<double>(mean));
    }
  };

} // namespace snmalloc

#endif // SNMALLOC_PROFILE
