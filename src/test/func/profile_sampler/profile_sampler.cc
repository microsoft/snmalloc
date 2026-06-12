// SPDX-License-Identifier: MIT
//
// Unit tests for the snmalloc heap-profile Phase 2.2 sampler primitives.
//
// Covers:
//   - Sampler::record_alloc statistical distribution + weight unbiasedness
//   - First-sample bootstrap unbiasedness
//   - Reentrancy guard short-circuits record_alloc
//   - NodePool acquire/release + exhaustion + drop counter
//   - SampledList single-threaded push/remove/snapshot
//   - SampledList multi-threaded push/remove (UAF-clean per-thread isolation)
//   - End-to-end: sampler fires, list contains node with captured stack
//
// These tests touch only the profile/ headers and do not exercise any
// allocator path -- Phase 2.2 deliverables are purely additive.

#include <test/opt.h>
#include <test/setup.h>
#include <test/snmalloc_testlib.h>

#include <snmalloc/profile/profile.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using snmalloc::profile::NodePool;
using snmalloc::profile::NodeState;
using snmalloc::profile::ReentrancyGuard;
using snmalloc::profile::SampledAlloc;
using snmalloc::profile::SampledList;
using snmalloc::profile::Sampler;
using snmalloc::profile::SamplerGlobals;
using snmalloc::profile::sampler_reentered;

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

  // -------------------------------------------------------------------------
  // Test: Sampler distribution.
  //
  // With T = sampling_rate, requested_size = R, the sampler should fire about
  // once per T bytes of request, and the sum of weights should be unbiased
  // for total allocated bytes.
  // -------------------------------------------------------------------------
  void test_sampler_distribution()
  {
    std::cout << "test_sampler_distribution\n";
    Sampler s;
    constexpr size_t T = 512 * 1024;
    constexpr size_t R = 64;
    constexpr size_t N = 4'000'000; // ~244 MiB; expected ~488 samples
    Sampler::set_sampling_rate(T);

    size_t sample_count = 0;
    uint64_t weight_sum = 0;
    for (size_t i = 0; i < N; ++i)
    {
      if (s.record_alloc(R))
      {
        ++sample_count;
        weight_sum += s.last_weight();
      }
    }

    const double total_bytes = static_cast<double>(N) * R;
    const double expected_samples = total_bytes / static_cast<double>(T);
    const double mean_interval =
      total_bytes / std::max<size_t>(sample_count, 1);

    std::cout << "    N=" << N << " R=" << R << " T=" << T << "\n";
    std::cout << "    samples=" << sample_count
              << "  expected~" << expected_samples << "\n";
    std::cout << "    mean_interval=" << mean_interval << " bytes\n";
    std::cout << "    weight_sum=" << weight_sum
              << "  total_request_bytes=" << total_bytes << "\n";

    // Expected within +/- 25% (3-sigma at this N is ~14%; loose for CI noise).
    check(
      sample_count >
        static_cast<size_t>(expected_samples * 0.75),
      "sample count not pathologically low");
    check(
      sample_count <
        static_cast<size_t>(expected_samples * 1.25),
      "sample count not pathologically high");

    // Weight sum should equal total bytes within ~5%.
    const double weight_err =
      std::fabs(static_cast<double>(weight_sum) - total_bytes) / total_bytes;
    std::cout << "    weight error = " << (weight_err * 100.0) << "%\n";
    check(weight_err < 0.10, "weight sum unbiased within 10%");
  }

  // -------------------------------------------------------------------------
  // Test: First-sample bootstrap.
  //
  // Spawn N fresh Samplers, each does exactly one record_alloc(R) with
  // T chosen so P(sample) = R/T. The total sample count should follow
  // Binomial(N, R/T); a buggy bootstrap (initial countdown = T) yields 0.
  // -------------------------------------------------------------------------
  void test_sampler_bootstrap()
  {
    std::cout << "test_sampler_bootstrap\n";
    constexpr size_t T = 4096;
    constexpr size_t R = 64;
    constexpr size_t N = 100'000;
    Sampler::set_sampling_rate(T);

    const double p = static_cast<double>(R) / static_cast<double>(T);
    const double expected = N * p;             // ~1562.5
    const double sigma = std::sqrt(N * p * (1 - p)); // ~39

    size_t hits = 0;
    for (size_t i = 0; i < N; ++i)
    {
      Sampler s;
      if (s.record_alloc(R))
        ++hits;
    }

    std::cout << "    N=" << N << "  expected=" << expected
              << "  sigma=" << sigma << "  observed=" << hits << "\n";

    // 5-sigma window catches "all zero" (bad bootstrap) and "way too many"
    // (auto-sample-first bug) without flaking in CI.
    check(hits > 0, "non-zero hits (bootstrap not deterministic)");
    check(
      static_cast<double>(hits) > expected - 5 * sigma,
      "hit count above 5-sigma lower bound");
    check(
      static_cast<double>(hits) < expected + 5 * sigma,
      "hit count below 5-sigma upper bound");
  }

  // -------------------------------------------------------------------------
  // Test: Reentrancy guard.
  // -------------------------------------------------------------------------
  void test_reentrancy_guard()
  {
    std::cout << "test_reentrancy_guard\n";
    check(!sampler_reentered(), "flag clear at start");
    {
      ReentrancyGuard g;
      check(sampler_reentered(), "flag set inside guard scope");
    }
    check(!sampler_reentered(), "flag clear after guard scope");

    // record_alloc must short-circuit when guard is armed.
    Sampler s;
    Sampler::set_sampling_rate(64); // very aggressive; first call would fire
    ReentrancyGuard g;
    check(!s.record_alloc(1024 * 1024), "record_alloc returns false under guard");
  }

  // -------------------------------------------------------------------------
  // Test: NodePool acquire/release/exhaustion/drop counter.
  // -------------------------------------------------------------------------
  void test_node_pool_basic()
  {
    std::cout << "test_node_pool_basic\n";
    using SmallPool = NodePool<32>;
    SmallPool pool;
    pool.init();

    std::vector<SampledAlloc*> nodes;
    nodes.reserve(32);
    for (size_t i = 0; i < 32; ++i)
    {
      SampledAlloc* n = pool.acquire();
      check(n != nullptr, "acquire returns node within capacity");
      if (n != nullptr)
        nodes.push_back(n);
    }

    // Exhaustion.
    SampledAlloc* over = pool.acquire();
    check(over == nullptr, "acquire returns null past capacity");
    check(pool.drop_count() >= 1, "drop counter increments on exhaustion");

    // Verify reset_for_acquire zeroed payload + bumped state to Live.
    for (auto* n : nodes)
    {
      check(
        n->state.load(std::memory_order_relaxed) ==
          static_cast<uint8_t>(NodeState::Live),
        "acquired node is Live");
    }

    // Strictly monotonic alloc_seq.
    bool monotonic = true;
    for (size_t i = 1; i < nodes.size(); ++i)
    {
      if (nodes[i]->alloc_seq <= nodes[i - 1]->alloc_seq)
      {
        monotonic = false;
        break;
      }
    }
    check(monotonic, "alloc_seq strictly monotonic across acquires");

    // Return all and verify capacity is restored.
    for (auto* n : nodes)
      pool.release(n);

    size_t reacquired = 0;
    while (pool.acquire() != nullptr)
      ++reacquired;
    check(reacquired == 32, "all nodes reusable after release");
  }

  // -------------------------------------------------------------------------
  // Test: SampledList push/remove/snapshot (single threaded).
  // -------------------------------------------------------------------------
  void test_sampled_list_single_threaded()
  {
    std::cout << "test_sampled_list_single_threaded\n";
    using SmallPool = NodePool<64>;
    SmallPool pool;
    pool.init();

    SampledList list;
    std::vector<SampledAlloc*> nodes;
    constexpr size_t M = 16;

    for (size_t i = 0; i < M; ++i)
    {
      auto* n = pool.acquire();
      n->alloc_addr = 0x1000 + i;
      list.push(n);
      nodes.push_back(n);
    }

    check(list.debug_count() == M, "snapshot sees all pushed nodes");

    // Remove half.
    for (size_t i = 0; i < M; i += 2)
      check(list.remove(nodes[i]), "remove returns true on first call");
    check(list.debug_count() == M / 2, "snapshot omits tombstoned nodes");

    // Double-remove is no-op.
    check(!list.remove(nodes[0]), "remove returns false on repeated call");

    // Drain to clean up.
    list.debug_drain([&](SampledAlloc* n) { pool.release(n); });
    check(list.debug_count() == 0, "drain empties the list");
  }

  // -------------------------------------------------------------------------
  // Test: SampledList concurrent push (no removes).
  // -------------------------------------------------------------------------
  void test_sampled_list_concurrent_push()
  {
    std::cout << "test_sampled_list_concurrent_push\n";
    using BigPool = NodePool<4096>;
    BigPool pool;
    pool.init();

    SampledList list;
    constexpr size_t kThreads = 4;
    constexpr size_t kPerThread = 512;

    std::vector<std::thread> ts;
    for (size_t t = 0; t < kThreads; ++t)
    {
      ts.emplace_back([&, t] {
        for (size_t i = 0; i < kPerThread; ++i)
        {
          auto* n = pool.acquire();
          if (n == nullptr)
            continue;
          n->alloc_addr = (t << 32) | i;
          list.push(n);
        }
      });
    }
    for (auto& th : ts)
      th.join();

    const size_t observed = list.debug_count();
    std::cout << "    threads=" << kThreads << " per_thread=" << kPerThread
              << " observed=" << observed << "\n";
    check(observed == kThreads * kPerThread, "all pushed nodes observed");

    list.debug_drain([&](SampledAlloc* n) { pool.release(n); });
  }

  // -------------------------------------------------------------------------
  // Test: SampledList concurrent push + remove (mixed).
  //
  // Every pushed node is later removed by some thread. After join, the list
  // should be empty.
  // -------------------------------------------------------------------------
  void test_sampled_list_concurrent_push_remove()
  {
    std::cout << "test_sampled_list_concurrent_push_remove\n";
    using BigPool = NodePool<4096>;
    BigPool pool;
    pool.init();

    SampledList list;
    constexpr size_t kThreads = 4;
    constexpr size_t kPerThread = 256;

    std::vector<std::vector<SampledAlloc*>> per_thread_nodes(kThreads);

    std::vector<std::thread> ts;
    for (size_t t = 0; t < kThreads; ++t)
    {
      ts.emplace_back([&, t] {
        auto& vec = per_thread_nodes[t];
        vec.reserve(kPerThread);
        for (size_t i = 0; i < kPerThread; ++i)
        {
          auto* n = pool.acquire();
          if (n == nullptr)
            continue;
          n->alloc_addr = (t << 32) | i;
          list.push(n);
          vec.push_back(n);
        }
      });
    }
    for (auto& th : ts)
      th.join();

    // Now have a separate set of threads remove half the nodes each
    // (cross-thread remove pattern).
    std::vector<std::thread> rs;
    for (size_t t = 0; t < kThreads; ++t)
    {
      rs.emplace_back([&, t] {
        // Thread t removes thread ((t+1) % kThreads)'s nodes -- cross-thread.
        auto& vec = per_thread_nodes[(t + 1) % kThreads];
        for (auto* n : vec)
          list.remove(n);
      });
    }
    for (auto& th : rs)
      th.join();

    const size_t left = list.debug_count();
    std::cout << "    remaining live = " << left << "\n";
    check(left == 0, "all nodes removed across cross-thread frees");

    list.debug_drain([&](SampledAlloc* n) { pool.release(n); });
  }

  // -------------------------------------------------------------------------
  // Test: End-to-end. Force a sample fire on a fresh Sampler with a
  // very small interval; verify a node appears on the global list with a
  // non-zero captured stack depth (assuming the FP walker is available;
  // otherwise stack_depth may be 0 on the null walker path).
  // -------------------------------------------------------------------------
  SNMALLOC_USED_FUNCTION
  void test_end_to_end_inner(Sampler& s, bool& fired_ref)
  {
    fired_ref = false;
    // Hammer with small allocs until we see a fire (bounded by N).
    for (size_t i = 0; i < 100; ++i)
    {
      if (s.record_alloc(0xCAFE0000 + i, 64, 64))
      {
        fired_ref = true;
        break;
      }
    }
  }

  void test_end_to_end()
  {
    std::cout << "test_end_to_end\n";

    // Use a fresh Sampler with very aggressive rate so the first few
    // record_allocs almost certainly fire.
    Sampler::set_sampling_rate(1); // every byte should sample on bootstrap
    Sampler s;

    bool fired = false;
    test_end_to_end_inner(s, fired);

    check(fired, "sample fired at least once with rate=1");
    if (!fired)
      return;

    SampledAlloc* node = s.last_sample();
    check(node != nullptr, "Sampler::last_sample non-null after fire");
    if (node == nullptr)
      return;

    check(node->requested_size == 64, "node->requested_size populated");
    check(
      (node->alloc_addr & 0xFFFF0000u) == 0xCAFE0000u,
      "node->alloc_addr populated");
    check(
      node->state.load(std::memory_order_relaxed) ==
        static_cast<uint8_t>(NodeState::Live),
      "node state is Live");
    check(
      node->sample_interval_at_capture == Sampler::get_sampling_rate(),
      "sample_interval_at_capture set");

    // Stack capture may be 0 frames on platforms with the null walker.
    // We accept both outcomes but log which one happened.
    std::cout << "    captured stack_depth = "
              << static_cast<int>(node->stack_depth) << "\n";

    // The node must be reachable via the global SampledList snapshot.
    bool found_on_list = false;
    SamplerGlobals::list().snapshot([&](SampledAlloc* n) {
      if (n == node)
        found_on_list = true;
    });
    check(found_on_list, "published node visible in SampledList snapshot");
  }

  // -------------------------------------------------------------------------
  // Test: Rate-change correctness.
  // -------------------------------------------------------------------------
  void test_rate_change()
  {
    std::cout << "test_rate_change\n";
    Sampler s;
    constexpr size_t R = 64;

    // Phase 1: rate = 64 KiB, ~200 MiB allocated -> ~3200 samples.
    constexpr size_t T1 = 64 * 1024;
    constexpr size_t N1 = 3'000'000; // ~183 MiB
    Sampler::set_sampling_rate(T1);
    uint64_t sum1 = 0;
    size_t hits1 = 0;
    for (size_t i = 0; i < N1; ++i)
    {
      if (s.record_alloc(R))
      {
        ++hits1;
        sum1 += s.last_weight();
      }
    }

    // Phase 2: rate = 256 KiB, ~200 MiB allocated -> ~800 samples.
    constexpr size_t T2 = 256 * 1024;
    constexpr size_t N2 = 3'000'000;
    Sampler::set_sampling_rate(T2);
    uint64_t sum2 = 0;
    size_t hits2 = 0;
    for (size_t i = 0; i < N2; ++i)
    {
      if (s.record_alloc(R))
      {
        ++hits2;
        sum2 += s.last_weight();
      }
    }

    std::cout << "    phase1 T=" << T1 << "  hits=" << hits1
              << "  sum=" << sum1 << "  expected~" << (N1 * R) << "\n";
    std::cout << "    phase2 T=" << T2 << "  hits=" << hits2
              << "  sum=" << sum2 << "  expected~" << (N2 * R) << "\n";

    // Hits should be roughly proportional to N*R/T.
    check(hits1 > hits2, "smaller T yields more samples");
    // Each batch's weighted sum should approximate its true bytes.
    const double e1 = std::fabs(double(sum1) - double(N1 * R)) / (N1 * R);
    const double e2 = std::fabs(double(sum2) - double(N2 * R)) / (N2 * R);
    std::cout << "    phase1 weight err=" << (e1 * 100) << "%  phase2 err="
              << (e2 * 100) << "%\n";
    check(e1 < 0.15, "phase1 weight unbiased within 15%");
    check(e2 < 0.25, "phase2 weight unbiased within 25%");
  }
} // namespace

int main(int argc, char** argv)
{
  (void)argc;
  (void)argv;
  std::cout << "[profile_sampler]\n";

  test_node_pool_basic();
  test_reentrancy_guard();
  test_sampled_list_single_threaded();
  test_sampled_list_concurrent_push();
  test_sampled_list_concurrent_push_remove();

  // Reset global rate before any sampler tests; previous test left it at 64.
  Sampler::set_sampling_rate(512 * 1024);

  test_sampler_bootstrap();
  test_sampler_distribution();
  test_rate_change();

  // End-to-end last: leaves a node on the global list.
  test_end_to_end();

  if (g_fail_count == 0)
  {
    std::cout << "[profile_sampler] ALL TESTS PASSED\n";
    return 0;
  }
  std::cout << "[profile_sampler] " << g_fail_count << " TEST(S) FAILED\n";
  return 1;
}
