// SPDX-License-Identifier: MIT
//
// Phase 7.3 — validate that compiling the heap-profile lazy provider into
// the build adds zero bytes to slab metadata when SNMALLOC_PROFILE is OFF,
// and that the dealloc-side null-slot fast-path is well-predicted when
// profiling is ON but no samples ever fire (ticket 86ahrfybd).
//
// What this test asserts:
//
//   (1) Layout — compile-time.
//       a. `LazyArrayClientMetaDataProvider<T>::StorageType` is exactly one
//          pointer wide (the public contract from commonconfig.h).
//       b. `NoClientMetaDataProvider::StorageType` is the empty type, so
//          slab metadata that embeds it via SNMALLOC_NO_UNIQUE_ADDRESS pays
//          zero bytes.  Concretely:
//             sizeof(StandardConfig::PagemapEntry) ==
//             sizeof(StandardConfigClientMeta<NoClientMetaDataProvider>
//                    ::PagemapEntry)
//          which proves the lazy provider type is *defined* in the build
//          but isn't *instantiated* into the default config's metadata.
//       c. The Phase 7.1 cache-aligned `SamplerHotState` puts
//          `bytes_until_sample` at offset 0 within the hot struct.
//
//   (2) Sampler hot-path overhead — runtime.
//       With SNMALLOC_PROFILE on we benchmark 1M allocs of size 32 under
//       two regimes:
//         * `Sampler::set_sampling_rate(0)` — sampling disabled.
//         * `Sampler::set_sampling_rate(2^40)` — sampling on but the
//           per-thread countdown never crosses zero within 1M*32B, so the
//           slow path is not entered.
//       Both fast paths execute the same instructions; the lazy provider's
//       per-slab backing is never installed because no sample fires.
//       Assert that the ratio of ns/alloc between the two regimes stays
//       below 1.05 — i.e., the "profile on but no fires" path does not
//       suffer a branch-misprediction storm relative to "profile off".
//
// Build gate:
//   The runtime benchmark is wrapped in `#ifdef SNMALLOC_PROFILE`.  When
//   profiling is off the test compiles to a smoke pass and exercises only
//   the layout assertions (which hold in both build configurations).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <snmalloc/backend/globalconfig.h>
#include <snmalloc/profile/profile.h>
#include <snmalloc/profile/record.h>
#include <snmalloc/profile/sampler.h>
#include <snmalloc/snmalloc.h>
#include <test/setup.h>
#include <vector>

using snmalloc::profile::config_has_profile_slot_v;
using snmalloc::profile::ProfileSlot;
using snmalloc::profile::SampledAlloc;
using snmalloc::profile::Sampler;
using snmalloc::profile::SamplerGlobals;

namespace
{
  int g_fail_count = 0;

  void check(bool cond, const char* msg)
  {
    if (cond)
    {
      std::cout << "  PASS: " << msg << "\n";
    }
    else
    {
      std::cout << "  FAIL: " << msg << "\n";
      ++g_fail_count;
    }
  }

  // ---------------------------------------------------------------------------
  // Compile-time layout assertions.
  //
  // These don't require running anything — they fire at TU compile time.
  // Wrapped in a function for readability and to keep them adjacent to the
  // runtime asserts that depend on them.
  // ---------------------------------------------------------------------------
  void test_layout_static()
  {
    std::cout << "test_layout_static\n";

    // (1a) Lazy provider's per-slab inline footprint is exactly one
    // pointer. This is the contract every config-author leans on.
    using LazyT =
      snmalloc::LazyArrayClientMetaDataProvider<std::atomic<SampledAlloc*>>;
    static_assert(
      sizeof(LazyT::StorageType) == sizeof(void*),
      "LazyArrayClientMetaDataProvider::StorageType must be one pointer "
      "wide; widening it would balloon slab metadata for every profile-on "
      "config.");
    check(
      sizeof(LazyT::StorageType) == sizeof(void*),
      "LazyArrayClientMetaDataProvider::StorageType == sizeof(void*)");

    // (1b) NoClientMetaDataProvider's storage is the Empty type. When
    // FrontendSlabMetadata embeds it via SNMALLOC_NO_UNIQUE_ADDRESS it
    // takes zero bytes — which is what makes the lazy provider's mere
    // *presence* in the build zero-overhead for non-profile configs.
    using NoProv = snmalloc::NoClientMetaDataProvider;
    static_assert(
      std::is_same_v<NoProv::StorageType, snmalloc::Empty>,
      "NoClientMetaDataProvider::StorageType must remain Empty so the "
      "[[no_unique_address]] member in FrontendSlabMetadata collapses.");

    // (1b cont.) Two PagemapEntry types — the project default Config and
    // an explicit StandardConfigClientMeta<NoClientMetaDataProvider> —
    // are layout-identical.  Both use NoClientMetaDataProvider, so the
    // lazy provider type is compiled into the TU yet contributes nothing.
    using DefaultEntry = snmalloc::Config::PagemapEntry;
    using ExplicitNoProvConfig =
      snmalloc::StandardConfigClientMeta<snmalloc::NoClientMetaDataProvider>;
    using ExplicitEntry = ExplicitNoProvConfig::PagemapEntry;
    static_assert(
      sizeof(DefaultEntry) == sizeof(ExplicitEntry),
      "Project-default PagemapEntry size must match explicit no-provider "
      "config size — proves zero overhead when profiling is OFF.");
    check(
      sizeof(DefaultEntry) == sizeof(ExplicitEntry),
      "sizeof(Config::PagemapEntry) == sizeof(NoProvider config "
      "PagemapEntry)");

    // (1c) Phase 7.1: bytes_until_sample lives at offset 0 of the
    // cache-aligned hot struct.
    static_assert(
      Sampler::kBytesUntilSampleOffset == 0,
      "Phase 7.1: bytes_until_sample must be the first member of "
      "SamplerHotState (offset 0 within the cache-aligned region).");
    check(
      Sampler::kBytesUntilSampleOffset == 0,
      "Sampler::SamplerHotState::bytes_until_sample at offset 0");

    // Phase 7.1: the hot state struct should be cache-aligned.
    static_assert(
      alignof(Sampler::SamplerHotState) >= 64,
      "Phase 7.1: SamplerHotState alignment should be at least 64 bytes "
      "to avoid false-sharing with neighbouring sampler state.");
    check(
      alignof(Sampler::SamplerHotState) >= 64,
      "alignof(SamplerHotState) >= 64");
  }

#ifdef SNMALLOC_PROFILE
  // ---------------------------------------------------------------------------
  // Tight micro-benchmark of the malloc/free fast path under two sampler
  // regimes.  Not a microbenchmark in the strict sense (no CPU pinning, no
  // warm-up averaging) — a sanity gate on whether the profile-on path with
  // no samples firing is roughly the same cost as profile-off.
  //
  // Configured below: 1M alloc/free pairs of size 32.  We choose 32 because
  // it's the smallest small-sizeclass and exercises the busiest path in the
  // allocator (least amortisation of fixed overhead).
  // ---------------------------------------------------------------------------
  double bench_alloc_free_loop(size_t iterations)
  {
    // Heap-allocate buffer so we can also free in order — we want to
    // exercise both alloc and dealloc paths under the same regime.
    std::vector<void*> ptrs(iterations, nullptr);

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    for (size_t i = 0; i < iterations; ++i)
    {
      ptrs[i] = snmalloc::libc::malloc(32);
    }
    for (size_t i = 0; i < iterations; ++i)
    {
      snmalloc::libc::free(ptrs[i]);
    }
    const auto end = clock::now();

    const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    // Each iteration = 1 alloc + 1 free.
    return static_cast<double>(ns) / static_cast<double>(iterations);
  }

  void test_lazy_provider_zero_overhead_runtime()
  {
    std::cout << "test_lazy_provider_zero_overhead_runtime\n";

    constexpr size_t ITERATIONS = 1'000'000;

    // Warm-up: a single run primes the allocator state (first-touch
    // mappings, TLS sampler init) so the timed runs are comparable.
    Sampler::set_sampling_rate(0);
    (void)bench_alloc_free_loop(ITERATIONS / 10);

    // Profiling OFF (rate = 0): the sampler's slow path on first call
    // parks the per-thread counter at INT64_MAX/2 and the fast path then
    // bails immediately every subsequent call.  No SampledAlloc is ever
    // published, no lazy backing array is ever installed.
    Sampler::set_sampling_rate(0);
    const double ns_off = bench_alloc_free_loop(ITERATIONS);

    // Profiling ON but no fires (rate huge): the fast path executes the
    // subtract + compare on bytes_until_sample, takes the LIKELY branch
    // (the comment we added in sampler.h), and bails out.  Across 1M
    // allocs of 32B (32 MiB total) we are nowhere near the 2^40 byte
    // countdown.  The dealloc-side null-slot fast-path (find_profile_slot
    // returns nullptr because no lazy backing has ever been installed)
    // is exercised on every free.
    constexpr size_t HUGE_RATE = static_cast<size_t>(1) << 40;
    Sampler::set_sampling_rate(HUGE_RATE);
    const double ns_on = bench_alloc_free_loop(ITERATIONS);

    // Restore default before returning.
    Sampler::set_sampling_rate(SamplerGlobals::kDefaultSamplingRate);

    std::cout << "    profile-off ns/alloc = " << ns_off << "\n";
    std::cout << "    profile-on  ns/alloc = " << ns_on << "\n";
    const double ratio = (ns_off > 0) ? (ns_on / ns_off) : 1.0;
    std::cout << "    ratio (on/off)       = " << ratio << "\n";

    // 5% bound matches the task contract.  Under the rate=infinite regime
    // both passes do effectively the same work; the bound is generous to
    // absorb timing noise on a non-quiesced developer box.
    check(
      ratio < 1.05,
      "lazy provider + sampler fast-path overhead < 5% (no sample fires)");
  }
#endif // SNMALLOC_PROFILE
} // namespace

int main(int argc, char** argv)
{
  snmalloc::UNUSED(argc, argv);
  setup();

  std::cout << "[profile_overhead]\n";
#ifdef SNMALLOC_PROFILE
  std::cout
    << "  (SNMALLOC_PROFILE is defined: runtime overhead bench enabled)\n";
#else
  std::cout << "  (SNMALLOC_PROFILE is undefined: layout-only smoke pass)\n";
#endif

  test_layout_static();
#ifdef SNMALLOC_PROFILE
  test_lazy_provider_zero_overhead_runtime();
#endif

  if (g_fail_count == 0)
  {
    std::cout << "[profile_overhead] ALL TESTS PASSED\n";
    return 0;
  }
  std::cout << "[profile_overhead] " << g_fail_count << " TEST(S) FAILED\n";
  return 1;
}
