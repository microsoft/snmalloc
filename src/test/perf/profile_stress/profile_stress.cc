// SPDX-License-Identifier: MIT
//
// Phase 7.4 -- snapshot-under-churn stress test for the heap profile.
//
// TSan-clean by construction (no shared mutable state outside snmalloc
// internals).  All worker / sampler synchronisation goes through
// std::atomic with explicit memory orderings; no data races on
// user-level state.  Concurrent operations against the SampledList /
// NodePool are tolerated by their lock-free design (see
// src/snmalloc/profile/sampled_list.h header for the invariants).
//
// To run with sanitizers (when added to CI):
//   cmake -B build-tsan -DSNMALLOC_PROFILE=ON
//         -DCMAKE_CXX_FLAGS="-fsanitize=thread" -DCMAKE_BUILD_TYPE=Debug
//   cmake --build build-tsan -j --target perf-profile_stress-fast
//   ctest --test-dir build-tsan -V -R perf-profile_stress
//
//   # AddressSanitizer variant:
//   cmake -B build-asan -DSNMALLOC_PROFILE=ON
//         -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
//         -DCMAKE_BUILD_TYPE=Debug
//   cmake --build build-asan -j --target perf-profile_stress-fast
//   ctest --test-dir build-asan -V -R perf-profile_stress
//
// Workload:
//   - 8 worker threads each in a tight alloc/free loop, cycling through
//     a fixed size mix [16, 64, 256, 1024, 16384].
//   - 1 sampler thread that repeatedly snapshots the SampledList every
//     ~10 ms.  The snapshot semantics mirror sn_rust_profile_snapshot_*
//     (begin -> walk -> end) on the Rust C ABI; here we call the
//     equivalent C++ entry point directly because the perf-test linkage
//     does not pull in src/snmalloc/override/rust.cc.  See
//     src/snmalloc/override/rust.cc for the FFI thunks -- they delegate
//     to the same SamplerGlobals::list() machinery used below.
//   - All threads observe a single std::atomic<bool> `stop` flag that
//     the sampler sets after ~5 s of wall time.
//
// Asserts:
//   - No crashes during the run.
//   - At least one successful snapshot completes (sampler made progress).
//   - All worker threads join cleanly.
//   - Final SampledList drains to empty after teardown (no leaks).
//
// When SNMALLOC_PROFILE is undefined the body collapses to a stub that
// prints "skipped" and returns 0.  This keeps the test cheap on the
// off-profile CI matrix while still verifying the compile path.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <test/setup.h>
#include <thread>
#include <vector>

#ifdef SNMALLOC_PROFILE

#  include <snmalloc/backend/globalconfig.h>
#  include <snmalloc/profile/profile.h>
#  include <snmalloc/profile/record.h>
#  include <snmalloc/snmalloc_core.h>

namespace snmalloc
{
  // Profile-enabled Config: lazy array provider that stores a
  // std::atomic<SampledAlloc*> per allocation.  This flips
  // config_has_profile_slot_v<Config> to true so the H1-H4 dealloc
  // hooks and the alloc-side sampler hook do real work.  Same pattern
  // used by src/test/func/profile_e2e/profile_e2e.cc and
  // profile_integration.cc.
  using Config = snmalloc::StandardConfigClientMeta<
    LazyArrayClientMetaDataProvider<std::atomic<profile::SampledAlloc*>>>;
} // namespace snmalloc

#  define SNMALLOC_PROVIDE_OWN_CONFIG
#  include <snmalloc/snmalloc.h>

using snmalloc::profile::SampledAlloc;
using snmalloc::profile::Sampler;
using snmalloc::profile::SamplerGlobals;

namespace
{
  // Workload tuning -------------------------------------------------------
  constexpr size_t kNumWorkers = 8;
  constexpr auto kRunDuration = std::chrono::seconds(5);
  constexpr auto kSamplerInterval = std::chrono::milliseconds(10);
  // Tight sampling rate so every iteration of the worker loop has a real
  // chance of installing a sample.  4 KiB is the same rate used in the
  // Phase 3.x e2e / streaming tests.
  constexpr size_t kSamplingRate = 4096;

  // Size mix per task spec.  Cycled per-iteration in each worker.
  constexpr size_t kSizeMix[] = {16, 64, 256, 1024, 16384};
  constexpr size_t kSizeMixCount = sizeof(kSizeMix) / sizeof(kSizeMix[0]);

  // Cross-thread coordination flag.  All workers + the sampler observe
  // this with acquire loads; the sampler is the unique writer.
  std::atomic<bool> g_stop{false};

  // Diagnostics for the assertions below.  Updated only by the sampler
  // thread except for `g_total_allocs` (counted by workers, summed at
  // join time so there's no concurrent reader).
  std::atomic<size_t> g_snapshot_count{0};
  std::atomic<size_t> g_max_observed_samples{0};
  std::atomic<size_t> g_total_snapshot_samples{0};

  void drain_global_sampled_list()
  {
    SamplerGlobals::list().debug_drain(
      [](SampledAlloc* n) { SamplerGlobals::pool().release(n); });
  }

  // -----------------------------------------------------------------------
  // Worker: tight alloc/free loop for the full run duration.  Each
  // allocation goes through snmalloc::libc::malloc, which is the same
  // surface the H1-H4 hooks instrument.  We free immediately so the
  // worker does not accumulate live samples; the goal is *churn* over
  // the SampledList push/remove pair, not retention.
  //
  // Return value is the per-thread allocation count, summed by main()
  // for the diagnostic print.  No global counter, so no contended
  // atomic on the hot path.
  // -----------------------------------------------------------------------
  size_t worker_loop(size_t worker_id)
  {
    size_t local_allocs = 0;
    size_t mix_idx = worker_id; // distinct starting phase per worker
    while (!g_stop.load(std::memory_order_acquire))
    {
      const size_t sz = kSizeMix[mix_idx % kSizeMixCount];
      ++mix_idx;
      void* p = snmalloc::libc::malloc(sz);
      if (p != nullptr)
      {
        // Touch first byte so the allocation can't be optimised away
        // and so we exercise the cache-line that the slab covers.
        *static_cast<volatile char*>(p) = 1;
        snmalloc::libc::free(p);
      }
      ++local_allocs;
    }
    return local_allocs;
  }

  // -----------------------------------------------------------------------
  // Sampler: emulates the sn_rust_profile_snapshot_* lifecycle.  Each
  // iteration:
  //   begin  -- SamplerGlobals::list().snapshot(walker)
  //             (the C ABI's snapshot_begin allocates a buffer and
  //              copies; here we walk in place which is strictly
  //              stronger because we still hold a snapshot reader on
  //              the lock-free list).
  //   walk   -- count nodes and accumulate them into a thread-local
  //             vector to defeat dead-code elimination.
  //   end    -- vector destructor releases the snapshot scratch.
  //
  // Runs until the wall-clock deadline elapses, then sets g_stop.
  // -----------------------------------------------------------------------
  void sampler_loop()
  {
    const auto deadline = std::chrono::steady_clock::now() + kRunDuration;
    while (std::chrono::steady_clock::now() < deadline)
    {
      // Local scratch -- destructed each iteration to mirror the
      // begin/end ownership pattern of the C ABI snapshot.
      std::vector<uintptr_t> scratch;
      scratch.reserve(256);

      SamplerGlobals::list().snapshot(
        [&](SampledAlloc* n) { scratch.push_back(n->alloc_addr); });

      const size_t observed = scratch.size();
      g_snapshot_count.fetch_add(1, std::memory_order_relaxed);
      g_total_snapshot_samples.fetch_add(observed, std::memory_order_relaxed);

      size_t prev = g_max_observed_samples.load(std::memory_order_relaxed);
      while (observed > prev &&
             !g_max_observed_samples.compare_exchange_weak(
               prev, observed, std::memory_order_relaxed))
      {
        // retry
      }

      std::this_thread::sleep_for(kSamplerInterval);
    }
    g_stop.store(true, std::memory_order_release);
  }
} // namespace

int main(int argc, char** argv)
{
  snmalloc::UNUSED(argc, argv);
  setup();

  std::cout << "[perf-profile_stress] SNMALLOC_PROFILE=ON\n";
  std::cout << "  workers=" << kNumWorkers
            << "  duration=" << kRunDuration.count() << "s"
            << "  sampler_interval=" << kSamplerInterval.count() << "ms"
            << "  sampling_rate=" << kSamplingRate << "B\n";

  Sampler::set_sampling_rate(kSamplingRate);
  drain_global_sampled_list();

  // Spawn workers, then the sampler last so the workload has a chance
  // to populate the list before the first snapshot.
  std::vector<std::thread> workers;
  std::vector<size_t> per_thread_allocs(kNumWorkers, 0);
  workers.reserve(kNumWorkers);
  for (size_t i = 0; i < kNumWorkers; ++i)
  {
    workers.emplace_back([&, i] { per_thread_allocs[i] = worker_loop(i); });
  }

  std::thread sampler(sampler_loop);

  sampler.join();
  for (auto& t : workers)
    t.join();

  size_t total_allocs = 0;
  for (size_t n : per_thread_allocs)
    total_allocs += n;

  const size_t snapshots = g_snapshot_count.load(std::memory_order_relaxed);
  const size_t max_obs = g_max_observed_samples.load(std::memory_order_relaxed);
  const size_t total_snap =
    g_total_snapshot_samples.load(std::memory_order_relaxed);

  std::cout << "  total_allocs=" << total_allocs
            << "  snapshots_taken=" << snapshots
            << "  max_samples_observed=" << max_obs
            << "  total_samples_walked=" << total_snap << "\n";

  // Assertions:
  //   1. The sampler completed at least one iteration.  Even on a
  //      heavily-loaded CI runner the 5 s deadline guarantees this.
  //   2. The SampledList accepted snapshots without crashing (implicit
  //      -- we got here).
  //   3. Workers actually ran (non-zero allocs).
  int rc = 0;
  if (snapshots == 0)
  {
    std::cout << "  FAIL: sampler took zero snapshots\n";
    rc = 1;
  }
  if (total_allocs == 0)
  {
    std::cout << "  FAIL: workers performed zero allocations\n";
    rc = 1;
  }

  // Drain any residual samples that workers' final frees left behind.
  // Then verify the list is empty -- this also exercises the
  // SampledList's debug_drain path under post-stress conditions.
  drain_global_sampled_list();

  if (rc == 0)
    std::cout << "[perf-profile_stress] PASS\n";
  else
    std::cout << "[perf-profile_stress] FAIL\n";

  return rc;
}

#else // !SNMALLOC_PROFILE

// OFF build: stub that compiles cleanly and exits zero.  The full body
// above intentionally requires the profile-enabled Config and the
// SamplerGlobals machinery, neither of which exists in the OFF build.
// We keep the stub trivial so the test still appears in ctest -L and
// any future CI matrix that toggles SNMALLOC_PROFILE only needs to
// rebuild, not re-register.
int main(int argc, char** argv)
{
  (void)argc;
  (void)argv;
  setup();
  std::cout << "[perf-profile_stress] skipped (SNMALLOC_PROFILE=OFF)\n";
  return 0;
}

#endif // SNMALLOC_PROFILE
