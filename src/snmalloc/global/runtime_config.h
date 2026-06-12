// SPDX-License-Identifier: MIT
//
// Runtime tunables (Phase 9.7).
//
// Centralises three previously-hardcoded knobs behind a single
// process-wide atomic-backed singleton:
//
//   * sample_interval_bytes  -- mean Poisson interval for the heap
//                               profiler.  Mirrored back into
//                               `snmalloc::profile::SamplerGlobals`
//                               via `Sampler::set_sampling_rate` so
//                               the sampler hot-path is unchanged
//                               (one atomic load per slow-path entry,
//                               i.e. ~1-in-512-KiB).
//
//   * decay_rate_ms          -- target window for returning unused
//                               chunks to the OS.  Producers of
//                               commit / decommit decisions in the
//                               backend should consult this value
//                               via `RuntimeConfig::decay_rate_ms()`
//                               in their slow path.  At the 9.7
//                               scaffold stage the setter is wired
//                               but the consumer is left for a
//                               follow-up ticket (the existing
//                               decay path is entangled with the
//                               `Range` template stack and a
//                               point-fix risks regressions); the
//                               getter / setter / FFI surface is
//                               in place so consumers can be added
//                               without churning the C ABI.
//
//   * max_local_cache_bytes  -- per-thread local-cache cap.  Same
//                               status as decay_rate_ms: storage +
//                               getter / setter / FFI ready, the
//                               read-side hook in the per-thread
//                               cache is a follow-up.
//
// The class is a header-only static-method facade over three
// function-local `std::atomic` singletons -- function-local because
// that defers construction until the first call, side-stepping any
// global-initialisation order dependency with the rest of snmalloc
// (which itself relies on careful first-touch initialisation of its
// per-thread allocator state).
//
// All operations are lock-free, wait-free, and safe to invoke from
// any thread at any point in the process lifetime, including before
// the first allocation.
//
// This header is intentionally POD-free: it carries only static
// methods and the `kDefault*` constants.  The C ABI shims in
// `override/runtime_config.cc` are the consumer-facing surface for
// non-C++ callers (notably the Rust binding in `snmalloc-rs`).

#pragma once

#include <atomic>
#include <cstdint>

namespace snmalloc
{
  /**
   * Runtime-settable allocator tunables.  See file header for the
   * full contract.  All methods are static; the class is a singleton
   * facade over three function-local atomics.
   */
  class RuntimeConfig
  {
  public:
    /// Default mean sampling interval, in bytes.  Matches
    /// `snmalloc::profile::SamplerGlobals::kDefaultSamplingRate`
    /// (512 KiB -- tcmalloc parity).  Kept in lockstep with the
    /// sampler default so callers that read the tunable before any
    /// override see the same value the sampler is actually using.
    static constexpr uint64_t kDefaultSampleIntervalBytes =
      static_cast<uint64_t>(512) * 1024;

    /// Default decay window, in milliseconds.  Picked to match the
    /// "tens of milliseconds" cadence the snmalloc README documents
    /// for chunk return; consumers in the backend may treat 0 as
    /// "decay immediately" once the read-side hook lands.
    static constexpr uint32_t kDefaultDecayRateMs = 50u;

    /// Default per-thread local-cache cap, in bytes.  Picked to
    /// match the existing soft upper bound used by the slab
    /// front-end (~1 MiB per thread); consumers that want a tighter
    /// cap for memory-constrained deployments can shrink it via
    /// `set_max_local_cache_bytes`.
    static constexpr uint64_t kDefaultMaxLocalCacheBytes =
      static_cast<uint64_t>(1) * 1024 * 1024;

    /**
     * Get the current mean sampling interval, in bytes.  Zero means
     * "sampling disabled".  Lock-free; safe from any thread.
     */
    [[nodiscard]] static uint64_t sample_interval_bytes() noexcept
    {
      return sample_interval_storage().load(std::memory_order_acquire);
    }

    /**
     * Set the mean sampling interval, in bytes.  Zero disables
     * sampling.  The new value is published with release ordering
     * so a subsequent acquire-load on any thread sees it.
     */
    static void set_sample_interval_bytes(uint64_t bytes) noexcept
    {
      sample_interval_storage().store(bytes, std::memory_order_release);
    }

    /**
     * Get the current chunk decay window, in milliseconds.  Zero
     * is a valid value and is interpreted by the backend (once
     * wired) as "decay immediately".  Lock-free; safe from any
     * thread.
     */
    [[nodiscard]] static uint32_t decay_rate_ms() noexcept
    {
      return decay_rate_storage().load(std::memory_order_acquire);
    }

    /**
     * Set the chunk decay window, in milliseconds.  Currently
     * stored only; the backend read-side hook is a follow-up.
     */
    static void set_decay_rate_ms(uint32_t milliseconds) noexcept
    {
      decay_rate_storage().store(milliseconds, std::memory_order_release);
    }

    /**
     * Get the current per-thread local-cache cap, in bytes.
     * Lock-free; safe from any thread.
     */
    [[nodiscard]] static uint64_t max_local_cache_bytes() noexcept
    {
      return max_local_cache_storage().load(std::memory_order_acquire);
    }

    /**
     * Set the per-thread local-cache cap, in bytes.  Currently
     * stored only; the per-thread cache read-side hook is a
     * follow-up.
     */
    static void set_max_local_cache_bytes(uint64_t bytes) noexcept
    {
      max_local_cache_storage().store(bytes, std::memory_order_release);
    }

  private:
    // Function-local statics: lazy-initialised on first call.  This
    // is what gives `RuntimeConfig` its "always safe to call, even
    // before the first allocation" property -- there is no global
    // construction order to worry about; the atomic is brought into
    // existence by whichever thread reaches the accessor first, and
    // the C++17 magic-statics guarantee makes that thread-safe.
    static std::atomic<uint64_t>& sample_interval_storage() noexcept
    {
      static std::atomic<uint64_t> v{kDefaultSampleIntervalBytes};
      return v;
    }

    static std::atomic<uint32_t>& decay_rate_storage() noexcept
    {
      static std::atomic<uint32_t> v{kDefaultDecayRateMs};
      return v;
    }

    static std::atomic<uint64_t>& max_local_cache_storage() noexcept
    {
      static std::atomic<uint64_t> v{kDefaultMaxLocalCacheBytes};
      return v;
    }
  };
} // namespace snmalloc
