// SPDX-License-Identifier: MIT
//
// Heap profiler -- per-thread Poisson sampler.
//
// Phase 2.2 of the heap-profiling milestone (ClickUp 86ahrfw19). Purely
// additive: not yet wired into any allocator path, not gated on a profile
// build flag, no behaviour change to existing code.
//
// Math: byte-counted Poisson process. Fast path is one signed-int subtract
// + one branch. Slow path draws Exp(rate) using a branchless polynomial
// approximation of log2 (no libm). See
//   .claude/research/heap-profiling/profile-weight.md
// for the weight formula contract.
//
// Per-sample side-effects (wired at sample fire):
//   1. Re-entrancy check via ReentrancyGuard.
//   2. NodePool::acquire to get a SampledAlloc; drop on exhaustion.
//   3. Stack capture via the profile FramePointerWalker.
//   4. Populate SampledAlloc payload.
//   5. SampledList::push to publish.

#pragma once

#include "../ds_core/defines.h"
#include "../pal/pal_stack_walker.h"
#include "node_pool.h"
#include "reentrancy_guard.h"
#include "sampled_alloc.h"
#include "sampled_list.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#  if defined(_MSC_VER)
#    include <intrin.h>
#  else
#    include <x86intrin.h>
#  endif
#endif

// Phase 7.1: cache-line width used for `SamplerHotState` alignment so the
// per-thread fast-path counter does not false-share with neighbouring data.
// Apple Silicon (and other 64-bit ARM platforms shipped by Apple) uses a
// 128-byte L1 line; everything else we care about today is 64 bytes.
#ifndef SNMALLOC_CACHE_LINE_SIZE
#  if defined(__APPLE__) && defined(__aarch64__)
#    define SNMALLOC_CACHE_LINE_SIZE 128
#  else
#    define SNMALLOC_CACHE_LINE_SIZE 64
#  endif
#endif

namespace snmalloc::profile
{
  /**
   * Raw per-thread fast-path countdown (Bundle tweak 2, ticket
   * 86aj0jfwh).
   *
   * Promoting the hot counter out of `Sampler` to a namespace-scope
   * `thread_local int64_t` lets the inlined alloc-side hook
   * (`profile::record_alloc<Config>` in profile/record.h) materialise
   * its fast path as a single TLS subtract + signed compare, with no
   * `Sampler`-typed TLS lookup at all on the common branch.  The
   * slow path indirects through `tl_sampler` (cheap, ~1-in-512-KiB).
   *
   * Initialisation convention: `0` means "uninitialised; bootstrap on
   * first call".  The fast path's `<= 0` branch funnels the very first
   * allocation on a thread into the slow path, which then draws an
   * Exp(rate) interval and seeds the counter via
   * `record_alloc_slow_namespace_tls`.
   *
   * The Sampler class retains its own `hot_.bytes_until_sample` for
   * member-API callers (unit tests construct stack-allocated `Sampler`
   * instances and expect per-instance counter state).  The production
   * `tl_sampler` singleton is bypassed on the fast path.
   */
  inline thread_local int64_t bytes_until_sample = 0;

  /**
   * Global state shared across all per-thread Sampler instances.
   *
   * Lives in an inline variable so it has one definition across TUs (C++17).
   * `set_sampling_rate(0)` disables sampling globally; existing per-thread
   * countdowns remain valid (sample_interval_at_capture is recorded per
   * fire so a later rate change does not mis-weight already-captured
   * samples).
   */
  struct SamplerGlobals
  {
    /// Default mean sampling interval in bytes (matches tcmalloc default).
    static constexpr size_t kDefaultSamplingRate = 512 * 1024;

    static std::atomic<size_t>& sampling_rate() noexcept
    {
      static std::atomic<size_t> rate{kDefaultSamplingRate};
      return rate;
    }

    /// Global pool of SampledAlloc nodes. One per process.
    static NodePool<>& pool() noexcept
    {
      static NodePool<> p;
      return p;
    }

    /// Global list of currently-sampled allocations. One per process.
    static SampledList& list() noexcept
    {
      static SampledList l;
      return l;
    }

    /// Process-wide thread salt for PRNG seeding (XOR mixed in).
    static std::atomic<uint64_t>& thread_salt() noexcept
    {
      static std::atomic<uint64_t> salt{0xDEADBEEFCAFEBABEULL};
      return salt;
    }
  };

  /**
   * Per-thread Poisson sampler.
   *
   * Cost model (fast path):
   *   - one int64_t subtract on bytes_until_sample_
   *   - one signed compare + conditional branch
   *   - return false
   * Hits the slow path once per ~sampling_rate bytes (default 512 KiB).
   *
   * Slow path (~once per 512 KiB):
   *   - re-entrancy check
   *   - xoshiro256** step (~5 cycles)
   *   - exponential draw via libm `log` (~20 cycles)
   *   - weight + counter update
   *   - on sample fire: pool acquire + stack walk + list push
   */
  class Sampler
  {
  public:
    Sampler() noexcept = default;
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    /**
     * Hot path. Returns true iff the current allocation was sampled.
     *
     * On true, the caller may read `last_sample()` to obtain the
     * SampledAlloc* that was published; on false, last_sample() returns
     * nullptr.
     *
     * Side-effect on fire: the SampledAlloc node is pushed onto the
     * global SampledList. The caller has no responsibility for the node's
     * lifetime -- it stays on the list until the corresponding dealloc
     * hook removes it (Phase 3).
     */
    SNMALLOC_FAST_PATH_INLINE bool record_alloc(
      uintptr_t alloc_addr,
      size_t requested_size,
      size_t allocated_size) noexcept
    {
      // Phase 7.2 fast-path: a single TLS decrement + signed compare.
      //
      // Re-entrancy detection has been moved into `record_alloc_slow`
      // (below).  Skipping the check on the hot path saves one TLS load
      // and one mispredictable branch per allocation; the only behaviour
      // difference is that under re-entry the per-thread countdown is
      // permitted to tick negative until the slow path next fires.  The
      // slow path observes the negative counter, notices the re-entry
      // flag, and bails without resetting the counter -- so the next
      // sample fires immediately when the outer slow path exits, which
      // is the desired behaviour.  Sample weighting accounts for the
      // overshoot via `rate - hot_.bytes_until_sample + requested_size`
      // so accuracy is preserved.
      //
      // Bundle tweak 2 (86aj0jfwh): in production the alloc-side hook
      // in `record.h` operates on a namespace-scope TLS counter
      // (`bytes_until_sample`) and only calls into the Sampler on the
      // slow path.  This member entry point is preserved unchanged for
      // unit tests that exercise stack-allocated `Sampler` instances --
      // those want per-instance counter state, which the namespace TLS
      // cannot provide.
      hot_.bytes_until_sample -= static_cast<int64_t>(requested_size);
      // Fast-path stays in branch-predictor's favour: the vast majority of
      // allocations don't fire a sample (default 1-in-512KiB).
      if (SNMALLOC_LIKELY(hot_.bytes_until_sample > 0))
      {
        last_sample_ = nullptr;
        return false;
      }
      return record_alloc_slow(alloc_addr, requested_size, allocated_size);
    }

    /// Convenience overload for callers that only have the request size.
    SNMALLOC_FAST_PATH_INLINE bool record_alloc(size_t requested_size) noexcept
    {
      return record_alloc(0, requested_size, requested_size);
    }

    /**
     * Slow-path-only entry used by the namespace-TLS fast path
     * (`tl_record_alloc`, bundle tweak 2 - ticket 86aj0jfwh).
     *
     * The caller has already debited `requested_size` from the
     * namespace-scope `bytes_until_sample` and observed a non-positive
     * counter.  This entry mirrors the namespace TLS counter into
     * `hot_.bytes_until_sample` (so the Sampler's bootstrap / weight
     * maths see the post-debit value), runs the slow path
     * (re-entrancy check, bootstrap, weight math, pool acquire, stack
     * walk, list push), then writes the freshly-drawn next interval
     * back out via the `counter_inout` reference so the fast path can
     * resume.
     */
    SNMALLOC_SLOW_PATH bool record_alloc_from_namespace_tls(
      uintptr_t alloc_addr,
      size_t requested_size,
      size_t allocated_size,
      int64_t& counter_inout) noexcept
    {
      hot_.bytes_until_sample = counter_inout;
      const bool fired =
        record_alloc_slow(alloc_addr, requested_size, allocated_size);
      counter_inout = hot_.bytes_until_sample;
      return fired;
    }

    /**
     * Weight in bytes-of-request of the most recent sample. Valid only
     * immediately after record_alloc returned true.
     */
    [[nodiscard]] uint64_t last_weight() const noexcept
    {
      return weight_;
    }

    /**
     * Sampling interval that was in force at the moment of the last sample.
     * Persisted per-node on SampledAlloc::sample_interval_at_capture too.
     */
    [[nodiscard]] uint64_t last_interval() const noexcept
    {
      return interval_at_capture_;
    }

    /**
     * The SampledAlloc that was just published, or nullptr if the most
     * recent record_alloc returned false (or the pool was exhausted).
     */
    [[nodiscard]] SampledAlloc* last_sample() const noexcept
    {
      return last_sample_;
    }

    /**
     * Current value of the per-thread countdown. Test-only.
     */
    [[nodiscard]] int64_t debug_bytes_until_sample() const noexcept
    {
      return hot_.bytes_until_sample;
    }

    [[nodiscard]] bool debug_initialized() const noexcept
    {
      // Bootstrap state is now inferred from `interval_at_capture_`:
      // it is zero until the first successful slow-path completion, at
      // which point it is set to the active sampling rate (which is
      // strictly non-zero because rate == 0 short-circuits earlier in
      // the slow path).  Exposed for the unit tests that previously
      // observed the explicit `initialized_` flag.
      return interval_at_capture_ != 0;
    }

    /**
     * Set the global mean sampling interval, in bytes. 0 disables sampling.
     * Per-thread countdowns are not redrawn; the new rate takes effect
     * at each thread's next slow-path entry.
     */
    static void set_sampling_rate(size_t bytes) noexcept
    {
      SamplerGlobals::sampling_rate().store(bytes, std::memory_order_relaxed);
    }

    [[nodiscard]] static size_t get_sampling_rate() noexcept
    {
      return SamplerGlobals::sampling_rate().load(std::memory_order_relaxed);
    }

  private:
    SNMALLOC_SLOW_PATH bool record_alloc_slow(
      uintptr_t alloc_addr,
      size_t requested_size,
      size_t allocated_size) noexcept
    {
      // Re-entrancy short-circuit.  Moved here from the fast path so the
      // ~99.99% of allocations that never enter the slow path do not pay
      // a TLS load + branch.  When we get here under re-entry (e.g. the
      // stack walker mallocs a thread-local buffer on first use) the
      // counter is left negative; the next allocation will re-enter the
      // slow path which is fine -- re-entry is bounded by the outer
      // slow-path frame.
      if (SNMALLOC_UNLIKELY(sampler_reentered()))
      {
        last_sample_ = nullptr;
        return false;
      }

      const uint64_t rate =
        SamplerGlobals::sampling_rate().load(std::memory_order_relaxed);
      if (SNMALLOC_UNLIKELY(rate == 0))
      {
        // Sampling disabled. Keep the counter parked far in the future so
        // the fast path keeps returning false without re-entering here.
        // We do NOT touch `interval_at_capture_` here -- a later
        // re-enable of sampling will re-bootstrap naturally via the
        // first-sample branch below if the sampler was never bootstrapped.
        hot_.bytes_until_sample = INT64_MAX / 2;
        last_sample_ = nullptr;
        return false;
      }

      // Bundle tweak D (86aj0kdym): the per-Sampler bootstrap branch is
      // detected via `interval_at_capture_ == 0` instead of a dedicated
      // `initialized_` boolean.  `interval_at_capture_` is set to the
      // active sampling rate (always strictly positive in this branch)
      // immediately after a successful bootstrap, so it doubles as the
      // "already bootstrapped" signal.  This saves a member load + branch
      // every time the slow path is entered after the first sample (i.e.
      // every ~rate bytes for the lifetime of the thread).
      if (SNMALLOC_UNLIKELY(interval_at_capture_ == 0))
      {
        // First-sample bootstrap (research §4): the initial countdown is
        // itself drawn from Exp(rate). We do NOT auto-sample the first
        // allocation -- that would reintroduce the same bias from the
        // other direction.
        seed_prng_if_needed();
        hot_.bytes_until_sample = draw_exponential(rate, prng_step()) -
          static_cast<int64_t>(requested_size);
        // Mark bootstrapped.  `interval_at_capture_` is the published
        // "last sample's interval" -- not yet meaningful here because no
        // sample has fired, but `last_sample()` returns nullptr on this
        // path so observers can disambiguate.  Setting it to `rate`
        // guarantees we never re-enter the bootstrap branch.
        interval_at_capture_ = rate;
        if (hot_.bytes_until_sample > 0)
        {
          last_sample_ = nullptr;
          return false;
        }
        // First allocation is large enough to itself cross the threshold;
        // fall through and fire a sample naturally.
      }

      // Compute weight in bytes of request *before* updating the counter.
      // hot_.bytes_until_sample here is <= 0 (overshoot).
      // weight = rate + requested_size + (-hot_.bytes_until_sample)
      //        = rate - hot_.bytes_until_sample + requested_size
      //
      // Compute the signed sum in int64_t then narrow back to uint64_t
      // in one explicit cast so -Wsign-conversion doesn't fire on the
      // mixed-signedness intermediates.
      weight_ = static_cast<uint64_t>(
        static_cast<int64_t>(rate) -
        static_cast<int64_t>(hot_.bytes_until_sample) +
        static_cast<int64_t>(requested_size));
      interval_at_capture_ = rate;

      // Reset the countdown by drawing the next interval.
      hot_.bytes_until_sample += draw_exponential(rate, prng_step());

      // Now the fun part: claim a node, capture a stack, publish on the
      // global list. Wrap in ReentrancyGuard so any transitive allocator
      // calls from the stack walker (or NodePool's first-call mmap)
      // re-enter `record_alloc_slow`, see the re-entry flag in the
      // prologue check above, and bail out without further work.
      ReentrancyGuard guard;

      SampledAlloc* node = SamplerGlobals::pool().acquire();
      if (SNMALLOC_UNLIKELY(node == nullptr))
      {
        // Pool exhausted. The drop is recorded by the pool itself.
        last_sample_ = nullptr;
        return true; // sample fired logically, just not recorded
      }

      node->alloc_addr = alloc_addr;
      node->requested_size = requested_size;
      node->allocated_size = allocated_size;
      node->weight = weight_;
      node->sample_interval_at_capture = interval_at_capture_;
      node->tid = current_tid();

      // Skip one frame to drop record_alloc_slow itself from the trace.
      node->stack_depth = static_cast<uint8_t>(
        snmalloc::profile::stack_walk(node->stack, MaxStackFrames, 1));

      SamplerGlobals::list().push(node);
      last_sample_ = node;
      return true;
    }

    // ---- xoshiro256** ----------------------------------------------------
    SNMALLOC_FAST_PATH_INLINE uint64_t prng_step() noexcept
    {
      const uint64_t result = rotl(s_[1] * 5, 7) * 9;
      const uint64_t t = s_[1] << 17;
      s_[2] ^= s_[0];
      s_[3] ^= s_[1];
      s_[1] ^= s_[2];
      s_[0] ^= s_[3];
      s_[2] ^= t;
      s_[3] = rotl(s_[3], 45);
      // OR-in 1 ensures non-zero output so __builtin_clzll is defined.
      return result | 1;
    }

    static constexpr uint64_t rotl(uint64_t x, int k) noexcept
    {
      return (x << k) | (x >> (64 - k));
    }

    void seed_prng_if_needed() noexcept
    {
      if (SNMALLOC_LIKELY((s_[0] | s_[1] | s_[2] | s_[3]) != 0))
        return;
      const uint64_t a = read_cycle_counter();
      const uint64_t b = reinterpret_cast<uintptr_t>(&a); // stack address
      const uint64_t c = SamplerGlobals::thread_salt().fetch_add(
        0x9E3779B97F4A7C15ULL, std::memory_order_relaxed);
      // SplitMix64 expansion to four words.
      uint64_t z = a ^ b ^ c;
      // Ensure z != 0 so the SplitMix64 mixes don't all collapse to 0.
      if (z == 0)
        z = 0x9E3779B97F4A7C15ULL;
      for (int i = 0; i < 4; ++i)
      {
        z += 0x9E3779B97F4A7C15ULL;
        uint64_t y = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        y = (y ^ (y >> 27)) * 0x94D049BB133111EBULL;
        s_[i] = y ^ (y >> 31);
      }
      if ((s_[0] | s_[1] | s_[2] | s_[3]) == 0)
        s_[0] = 1;
    }

    static uint64_t read_cycle_counter() noexcept
    {
#if defined(__x86_64__) || defined(_M_X64)
      return static_cast<uint64_t>(__rdtsc());
#elif defined(__aarch64__)
      uint64_t v;
      __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
      return v;
#else
      // Fallback entropy: the address of a thread-local rather than a
      // stack local (`&x` trips -Wreturn-stack-address on 32-bit gcc).
      // The exact value doesn't matter -- only mixed into the PRNG seed
      // at construction time.
      thread_local uint64_t entropy = 0;
      return reinterpret_cast<uintptr_t>(&entropy);
#endif
    }

    /**
     * Draw X ~ Exp(mean) from a uniform `r != 0`.
     *
     * Identity:  X = -mean * ln(U), where U = (r >> 11) * 2^-53 in (0, 1].
     *
     * Uses libm `std::log`. The slow path fires at most once per ~`mean`
     * bytes of request, so the libm call is amortised to <<1 ns/alloc on
     * the fast path. We avoided libm in earlier drafts (out of worry about
     * reentrancy from inside allocator hot paths); in practice `log` on
     * every libm we care about is a pure leaf function with no allocation
     * and no global state. The `ReentrancyGuard` in record_alloc_slow
     * provides defence-in-depth either way.
     *
     * Conversion of `r` to a double in (0, 1]: take the top 53 bits as the
     * mantissa to avoid double-rounding; "(r >> 11) | 1" guarantees the
     * value is strictly positive so `log` never returns -inf.
     */
    SNMALLOC_FAST_PATH_INLINE static int64_t
    draw_exponential(uint64_t mean, uint64_t r) noexcept
    {
      const uint64_t bits = (r >> 11) | 1; // 53-bit mantissa, non-zero
      const double u =
        static_cast<double>(bits) * (1.0 / static_cast<double>(1ULL << 53));
      const double x = -std::log(u); // x in (0, ln(2^53)) ~ (0, 36.7)
      const double bytes = static_cast<double>(mean) * x;
      // +1 guarantees forward progress even when bytes rounds to zero.
      return static_cast<int64_t>(bytes) + 1;
    }

    static uint64_t current_tid() noexcept
    {
      // Use the address of a thread_local as a stable thread identity.
      // This avoids platform-specific syscalls in the sampler hot path
      // and is sufficient for downstream readers that just need to
      // distinguish threads.
      thread_local int tid_anchor = 0;
      return reinterpret_cast<uintptr_t>(&tid_anchor);
    }

  public:
    // ---- layout-exposed types (public for Phase 7.3 offset asserts) -----
    //
    // Phase 7.1: pull the per-thread fast-path counter into a dedicated
    // cache-line-aligned struct, with `bytes_until_sample` as the first
    // member.  Cache-line aligned so concurrent dealloc clears on the same
    // thread don't false-share with the sampler hot path.
    struct alignas(SNMALLOC_CACHE_LINE_SIZE) SamplerHotState
    {
      int64_t bytes_until_sample{0};
    };

    /// Phase 7.3 layout check: the hot counter is the first member of the
    /// hot state struct (offset 0 within the cache-aligned region).
    static constexpr size_t kBytesUntilSampleOffset =
      offsetof(SamplerHotState, bytes_until_sample);
    static_assert(
      kBytesUntilSampleOffset == 0,
      "Phase 7.1/7.3: bytes_until_sample must be the first member of "
      "SamplerHotState so it sits at offset 0 of the cache-aligned region");

  private:
    // ---- state ----------------------------------------------------------
    //
    // `hot_` is intentionally the first member of Sampler: when the TLS
    // sampler is itself cache-aligned (alignas(SamplerHotState) is
    // inherited via the SamplerHotState member), the hot counter lives in
    // its own cache line distinct from any colder Sampler state below.
    SamplerHotState hot_{};
    uint64_t s_[4]{0, 0, 0, 0};
    uint64_t weight_{0};
    uint64_t interval_at_capture_{0};
    SampledAlloc* last_sample_{nullptr};
  };

  /**
   * Per-thread sampler. Trivially destructible; lives in TLS.
   */
  inline thread_local Sampler tl_sampler;

  /**
   * Production alloc-side fast-path entry (bundle tweak 2, ticket
   * 86aj0jfwh).
   *
   * Called from `profile::record_alloc<Config>` in record.h.  The
   * fast-path body lives in a free function so the compiler sees a
   * pure namespace-TLS subtract + branch, with no `Sampler`-typed TLS
   * lookup on the common path.  Slow path indirects through the
   * thread-local `tl_sampler` and forwards into
   * `Sampler::record_alloc_slow` via the existing member entry.
   *
   * Returns true iff the current allocation was sampled (in which
   * case the caller may consult `tl_sampler.last_sample()` to obtain
   * the published SampledAlloc*).
   */
  SNMALLOC_FAST_PATH_INLINE bool tl_record_alloc(
    uintptr_t alloc_addr, size_t requested_size, size_t allocated_size) noexcept
  {
    // One TLS load + sub + store + branch on the common path.
    bytes_until_sample -= static_cast<int64_t>(requested_size);
    if (SNMALLOC_LIKELY(bytes_until_sample > 0))
      return false;

    // Slow path: enter the per-thread Sampler.  Pass the namespace TLS
    // counter by reference; the Sampler runs its slow-path machinery
    // and writes the freshly-drawn next interval back through the
    // reference so the fast path resumes seamlessly.
    return tl_sampler.record_alloc_from_namespace_tls(
      alloc_addr, requested_size, allocated_size, bytes_until_sample);
  }
} // namespace snmalloc::profile
