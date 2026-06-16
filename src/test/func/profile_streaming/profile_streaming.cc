// SPDX-License-Identifier: MIT
//
// Phase 5.1 streaming-mode broadcast test.
//
// `AllocationSampleList::broadcast()` is invoked from `record_alloc` for
// every sampled allocation, in addition to the existing SampledList
// install path.  This test exercises the broadcast end-to-end:
//
//   1. Build the profile-enabled `snmalloc::Config` (same pattern as
//      profile_e2e.cc / profile_integration.cc).
//   2. Register a static counter callback with the global
//      `AllocationSampleList`.
//   3. Drive a few hundred thousand allocations at a tight sampling
//      rate.
//   4. Assert the callback fired approximately the number of times
//      expected from a Poisson process at that rate (same 6-sigma
//      envelope used by the other profile tests).
//   5. Assert the callback observes the same per-sample payload that a
//      concurrent `SampledList::snapshot` would observe (size,
//      non-zero address, non-zero stack).
//   6. Unregister and confirm the broadcast stops firing.
//
// When SNMALLOC_PROFILE is undefined the alloc hook is a compile-time
// no-op and broadcast is never called: we degrade to a smoke test that
// just checks zero callbacks fire.

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <snmalloc/backend/globalconfig.h>
#include <snmalloc/profile/profile.h>
#include <snmalloc/profile/record.h>
#include <snmalloc/snmalloc_core.h>
#include <test/setup.h>
#include <vector>

namespace snmalloc
{
  // Profile-enabled Config: same pattern as the other profile tests.
  using Config = snmalloc::StandardConfigClientMeta<
    LazyArrayClientMetaDataProvider<std::atomic<profile::SampledAlloc*>>>;
} // namespace snmalloc

#define SNMALLOC_PROVIDE_OWN_CONFIG
#include <snmalloc/snmalloc.h>

using snmalloc::profile::AllocationSampleList;
using snmalloc::profile::config_has_profile_slot_v;
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

  void drain_global_sampled_list()
  {
    SamplerGlobals::list().debug_drain(
      [](SampledAlloc* n) { SamplerGlobals::pool().release(n); });
  }

  // -----------------------------------------------------------------------
  // Test callback: counts invocations and aggregates payload sanity flags.
  //
  // The callback is `noexcept` per the AllocationSampleCallback contract
  // and writes only to file-scope atomics -- no allocation, no I/O.
  // -----------------------------------------------------------------------
  std::atomic<size_t> g_cb_count{0};
  std::atomic<size_t> g_cb_zero_addr{0};
  std::atomic<size_t> g_cb_zero_stack{0};
  std::atomic<size_t> g_cb_bad_size{0};
  std::atomic<size_t> g_cb_expected_size{0};

  [[maybe_unused]] void counting_callback(const SampledAlloc& s) noexcept
  {
    g_cb_count.fetch_add(1, std::memory_order_relaxed);
    if (s.alloc_addr == 0)
      g_cb_zero_addr.fetch_add(1, std::memory_order_relaxed);
    if (s.stack_depth == 0)
      g_cb_zero_stack.fetch_add(1, std::memory_order_relaxed);
    if (s.requested_size != g_cb_expected_size.load(std::memory_order_relaxed))
      g_cb_bad_size.fetch_add(1, std::memory_order_relaxed);
  }

  // Second callback (used to assert multi-subscriber broadcast).
  std::atomic<size_t> g_cb2_count{0};

  [[maybe_unused]] void second_callback(const SampledAlloc&) noexcept
  {
    g_cb2_count.fetch_add(1, std::memory_order_relaxed);
  }

  void reset_counters() noexcept
  {
    g_cb_count.store(0, std::memory_order_relaxed);
    g_cb_zero_addr.store(0, std::memory_order_relaxed);
    g_cb_zero_stack.store(0, std::memory_order_relaxed);
    g_cb_bad_size.store(0, std::memory_order_relaxed);
    g_cb2_count.store(0, std::memory_order_relaxed);
  }

  // =========================================================================
  // Test 1: broadcast fires once per sampled allocation.
  //
  // At sampling rate R bytes and N allocs of S bytes each, the Poisson
  // expectation is N*S/R samples.  Assert the callback count lands in
  // the same +/- 6 sigma envelope used elsewhere in the profile suite.
  // =========================================================================
  void test_broadcast_fires_per_sample()
  {
    std::cout << "test_broadcast_fires_per_sample\n";
    drain_global_sampled_list();
    AllocationSampleList::global().clear_all();
    reset_counters();

#ifndef SNMALLOC_PROFILE
    // OFF build: broadcast never invoked; counter must remain at zero.
    constexpr size_t N = 1000;
    std::vector<void*> ptrs;
    ptrs.reserve(N);
    const int rc =
      AllocationSampleList::global().register_handler(counting_callback);
    check(
      rc == AllocationSampleList::kOk, "register_handler succeeds in OFF mode");
    for (size_t i = 0; i < N; ++i)
      ptrs.push_back(snmalloc::libc::malloc(64));
    for (auto* p : ptrs)
      snmalloc::libc::free(p);
    check(
      g_cb_count.load() == 0,
      "OFF build: broadcast callback never fires (hooks are compile-time "
      "no-ops)");
    AllocationSampleList::global().unregister_handler(counting_callback);
    return;
#else
    static_assert(
      config_has_profile_slot_v<snmalloc::Config>,
      "test config must carry the lazy SampledAlloc-slot provider");

    constexpr size_t SAMPLING_RATE = 4096; // 4 KiB -- generous sample count
    constexpr size_t OBJ_SIZE = 64;
    constexpr size_t N = 100'000;

    Sampler::set_sampling_rate(SAMPLING_RATE);
    g_cb_expected_size.store(OBJ_SIZE, std::memory_order_relaxed);

    const int rc =
      AllocationSampleList::global().register_handler(counting_callback);
    check(
      rc == AllocationSampleList::kOk,
      "register_handler succeeds for the first subscriber");
    check(
      AllocationSampleList::global().subscriber_count() == 1,
      "subscriber_count reflects one registered handler");

    std::vector<void*> ptrs;
    ptrs.reserve(N);
    for (size_t i = 0; i < N; ++i)
    {
      void* p = snmalloc::libc::malloc(OBJ_SIZE);
      ptrs.push_back(p);
    }

    const size_t cb_observed = g_cb_count.load(std::memory_order_relaxed);
    const size_t list_observed = SamplerGlobals::list().debug_count();
    const double expected = static_cast<double>(N) * OBJ_SIZE / SAMPLING_RATE;
    const double sigma = std::sqrt(expected);
    const double low = expected - 6 * sigma;
    const double high = expected + 6 * sigma;
    std::cout << "    callback fires = " << cb_observed
              << "  list samples = " << list_observed
              << "  expected ~= " << expected << "  (+/- 6 sigma = " << sigma
              << ")\n";

    check(
      static_cast<double>(cb_observed) >= low &&
        static_cast<double>(cb_observed) <= high,
      "callback count within 6 sigma of Poisson expectation");
    // Streaming broadcast should fire for every sample that was also
    // pushed onto the SampledList -- and conversely, no sample should
    // be broadcast without being on the list.  In practice these two
    // counters move in lockstep because the broadcast happens
    // immediately after the slot CAS in `record_alloc`.
    check(
      cb_observed == list_observed,
      "broadcast count matches the SampledList live count");
    check(
      g_cb_zero_addr.load() == 0, "every broadcast carries a non-zero address");
    check(
      g_cb_zero_stack.load() == 0,
      "every broadcast carries a non-zero stack depth");
    check(
      g_cb_bad_size.load() == 0,
      "every broadcast reports the expected requested_size");

    // Tear down: free everything, unregister, restore default rate.
    for (auto* p : ptrs)
      snmalloc::libc::free(p);

    const int urc =
      AllocationSampleList::global().unregister_handler(counting_callback);
    check(urc == AllocationSampleList::kOk, "unregister_handler succeeds");
    check(
      AllocationSampleList::global().subscriber_count() == 0,
      "subscriber_count returns to zero after unregister");

    drain_global_sampled_list();
    Sampler::set_sampling_rate(SamplerGlobals::kDefaultSamplingRate);
#endif // SNMALLOC_PROFILE
  }

  // =========================================================================
  // Test 2: after unregister the broadcast no longer fires.
  // =========================================================================
  void test_unregister_stops_broadcast()
  {
    std::cout << "test_unregister_stops_broadcast\n";
    drain_global_sampled_list();
    AllocationSampleList::global().clear_all();
    reset_counters();

#ifndef SNMALLOC_PROFILE
    check(true, "SNMALLOC_PROFILE undefined: skipping");
    return;
#else
    constexpr size_t SAMPLING_RATE = 4096;
    constexpr size_t OBJ_SIZE = 64;
    constexpr size_t N = 50'000;

    Sampler::set_sampling_rate(SAMPLING_RATE);
    g_cb_expected_size.store(OBJ_SIZE, std::memory_order_relaxed);

    AllocationSampleList::global().register_handler(counting_callback);

    std::vector<void*> ptrs;
    ptrs.reserve(N);
    for (size_t i = 0; i < N; ++i)
      ptrs.push_back(snmalloc::libc::malloc(OBJ_SIZE));

    const size_t before = g_cb_count.load();
    check(before > 0, "broadcast fired during registered window");

    // Unregister; subsequent allocs MUST NOT fire the callback.
    AllocationSampleList::global().unregister_handler(counting_callback);

    std::vector<void*> ptrs2;
    ptrs2.reserve(N);
    for (size_t i = 0; i < N; ++i)
      ptrs2.push_back(snmalloc::libc::malloc(OBJ_SIZE));

    const size_t after = g_cb_count.load();
    check(
      after == before, "no further callbacks fire after unregister_handler");

    for (auto* p : ptrs)
      snmalloc::libc::free(p);
    for (auto* p : ptrs2)
      snmalloc::libc::free(p);

    drain_global_sampled_list();
    Sampler::set_sampling_rate(SamplerGlobals::kDefaultSamplingRate);
#endif // SNMALLOC_PROFILE
  }

  // =========================================================================
  // Test 3: multi-subscriber fan-out.  Two registered handlers must both
  // see the same number of broadcasts.
  // =========================================================================
  void test_multi_subscriber()
  {
    std::cout << "test_multi_subscriber\n";
    drain_global_sampled_list();
    AllocationSampleList::global().clear_all();
    reset_counters();

#ifndef SNMALLOC_PROFILE
    check(true, "SNMALLOC_PROFILE undefined: skipping");
    return;
#else
    constexpr size_t SAMPLING_RATE = 4096;
    constexpr size_t OBJ_SIZE = 64;
    constexpr size_t N = 50'000;

    Sampler::set_sampling_rate(SAMPLING_RATE);
    g_cb_expected_size.store(OBJ_SIZE, std::memory_order_relaxed);

    AllocationSampleList::global().register_handler(counting_callback);
    AllocationSampleList::global().register_handler(second_callback);
    check(
      AllocationSampleList::global().subscriber_count() == 2,
      "subscriber_count reflects two registered handlers");

    std::vector<void*> ptrs;
    ptrs.reserve(N);
    for (size_t i = 0; i < N; ++i)
      ptrs.push_back(snmalloc::libc::malloc(OBJ_SIZE));

    const size_t c1 = g_cb_count.load();
    const size_t c2 = g_cb2_count.load();
    std::cout << "    cb1 = " << c1 << "  cb2 = " << c2 << "\n";
    check(c1 > 0, "first callback fired");
    check(c2 > 0, "second callback fired");
    check(
      c1 == c2,
      "both callbacks see identical broadcast counts (fan-out is atomic)");

    AllocationSampleList::global().unregister_handler(counting_callback);
    AllocationSampleList::global().unregister_handler(second_callback);

    for (auto* p : ptrs)
      snmalloc::libc::free(p);

    drain_global_sampled_list();
    Sampler::set_sampling_rate(SamplerGlobals::kDefaultSamplingRate);
#endif // SNMALLOC_PROFILE
  }

  // =========================================================================
  // Test 4: slot exhaustion.  Registering past the fixed capacity must
  // return kNoFreeSlot; unregistering then allows a new registration to
  // succeed.  Pure smoke test that does not depend on the profile build.
  // =========================================================================
  void test_slot_exhaustion()
  {
    std::cout << "test_slot_exhaustion\n";
    AllocationSampleList::global().clear_all();

    // Build a small stable of distinct callbacks.  kMaxSubscribers is
    // 4 today; registering five must yield exactly one kNoFreeSlot.
    using CB = snmalloc::profile::AllocationSampleCallback;
    CB cbs[] = {
      [](const SampledAlloc&) noexcept {},
      [](const SampledAlloc&) noexcept {},
      [](const SampledAlloc&) noexcept {},
      [](const SampledAlloc&) noexcept {},
      [](const SampledAlloc&) noexcept {},
    };

    int rcs[5];
    for (size_t i = 0; i < 5; ++i)
      rcs[i] = AllocationSampleList::global().register_handler(cbs[i]);

    size_t ok = 0;
    size_t fail = 0;
    for (int rc : rcs)
    {
      if (rc == AllocationSampleList::kOk)
        ++ok;
      else
        ++fail;
    }
    std::cout << "    ok = " << ok << "  no-free-slot = " << fail << "\n";
    check(
      ok == AllocationSampleList::kMaxSubscribers,
      "exactly kMaxSubscribers registrations succeed");
    check(fail == 1, "the (kMaxSubscribers+1)-th registration is rejected");

    // Reject null cb.
    check(
      AllocationSampleList::global().register_handler(nullptr) ==
        AllocationSampleList::kNoFreeSlot,
      "registering nullptr is rejected");

    // Tear down.
    for (size_t i = 0; i < 5; ++i)
    {
      if (rcs[i] == AllocationSampleList::kOk)
        AllocationSampleList::global().unregister_handler(cbs[i]);
    }
    AllocationSampleList::global().clear_all();
    check(
      AllocationSampleList::global().subscriber_count() == 0,
      "clear_all leaves the broadcaster empty");
  }
} // namespace

int main(int argc, char** argv)
{
  snmalloc::UNUSED(argc, argv);
  setup();

  std::cout << "[profile_streaming]\n";
#ifdef SNMALLOC_PROFILE
  std::cout << "  (SNMALLOC_PROFILE is defined: streaming hook is live)\n";
#else
  std::cout
    << "  (SNMALLOC_PROFILE is undefined: smoke-only, hooks compiled out)\n";
#endif

  test_broadcast_fires_per_sample();
  test_unregister_stops_broadcast();
  test_multi_subscriber();
  test_slot_exhaustion();

  if (g_fail_count == 0)
  {
    std::cout << "[profile_streaming] ALL TESTS PASSED\n";
    return 0;
  }
  std::cout << "[profile_streaming] " << g_fail_count << " TEST(S) FAILED\n";
  return 1;
}
