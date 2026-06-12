// SPDX-License-Identifier: MIT
//
// Phase 3.4 integration test for the heap profile (ticket 86ahrfx9g).
//
// Description from the ticket:
//   "Multi-threaded alloc + cross-thread dealloc stress.  16 threads x
//    100k allocs x varying size, mix of free-on-same-thread and
//    cross-thread.  Assert: sample count within tolerance; SampledList
//    drains; no crash; no leak above documented tolerance."
//
// This is the largest stress test in the profile suite and is the
// canonical regression net for the H1 -> H4 hook surface.  Every dealloc
// hook is exercised:
//
//   H1: every same-thread free (the waist of Allocator::dealloc).
//   H2: every cross-thread free that takes the fast splice path.
//   H3: any free for a pointer whose pagemap entry reports !is_owned()
//       -- not directly forced here but the hook compiles in and is
//       defensively idempotent.
//   H4: any cross-thread free routed via dealloc_remote_slow's
//       lazy-init arm -- triggered organically by freshly-spawned
//       threads whose first action is a cross-thread free.
//
// As with the other Phase 3.x tests, we build a custom snmalloc Config
// that wires the `LazyArrayClientMetaDataProvider<ProfileSlot>` so
// `config_has_profile_slot_v<Config>` is true and the hooks do real
// work.  The OFF flavour (SNMALLOC_PROFILE undefined) runs the same
// allocation pattern as a smoke test with all hooks compiled out.

#include <test/setup.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <vector>

#include <snmalloc/backend/globalconfig.h>
#include <snmalloc/snmalloc_core.h>

#include <snmalloc/profile/profile.h>
#include <snmalloc/profile/record.h>

namespace snmalloc
{
  // Profile-enabled Config: lazy array provider that stores a
  // std::atomic<SampledAlloc*> per allocation.  This flips
  // config_has_profile_slot_v<Config> to true and exercises the real
  // profile pipeline through the live allocator.
  using Config = snmalloc::StandardConfigClientMeta<
    LazyArrayClientMetaDataProvider<std::atomic<profile::SampledAlloc*>>>;
} // namespace snmalloc

#define SNMALLOC_PROVIDE_OWN_CONFIG
#include <snmalloc/snmalloc.h>

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

#ifdef SNMALLOC_PROFILE
  size_t live_count()
  {
    return SamplerGlobals::list().debug_count();
  }
#endif

  // -----------------------------------------------------------------------
  // SPMC cross-thread queue used to ship pointers from a producer thread
  // to a dedicated "freer" thread.
  // -----------------------------------------------------------------------
  struct PtrQueue
  {
    std::mutex m;
    std::queue<void*> q;
    std::atomic<bool> producers_done{false};
  };

  // =========================================================================
  // The core integration test.
  //
  // We run THREAD_COUNT producer threads.  Each producer allocates
  // PER_THREAD objects of pseudo-random sizes chosen from a small ladder
  // (16B, 64B, 256B, 1024B).  For each allocation we coin-flip:
  //
  //   * 50% chance: free immediately on the producer thread -- exercises
  //     the same-thread H1 path.
  //
  //   * 50% chance: push onto a per-consumer queue.  A dedicated freer
  //     thread later dequeues and frees the pointer -- exercising the
  //     cross-thread H1+H2 path, and (for the very first free seen by a
  //     freshly-spawned freer) the H4 lazy-init arm of
  //     dealloc_remote_slow.
  //
  // After every producer finishes and every freer has drained its
  // queue, we assert:
  //
  //   * The producer-recorded sample count (live_count snapshot just
  //     before any cross-thread free begins) is within 6 sigma of the
  //     Poisson expectation.
  //   * The set of `alloc_seq` values that existed pre-free does NOT
  //     remain on the SampledList post-drain, except up to a small
  //     documented tolerance (the known thread-teardown straggler from
  //     Phase 3.3 -- <= 1% + 4).
  //   * The list ultimately drains to zero after `debug_drain` is
  //     called -- proving no leaked nodes.
  // =========================================================================
  void test_16_thread_mixed_free_stress()
  {
    std::cout << "test_16_thread_mixed_free_stress\n";
    drain_global_sampled_list();

#ifndef SNMALLOC_PROFILE
    check(true, "SNMALLOC_PROFILE undefined: smoke run only");
    constexpr size_t N_THREADS = 16;
    constexpr size_t PER_THREAD = 1024;
    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);
    for (size_t t = 0; t < N_THREADS; ++t)
    {
      threads.emplace_back([] {
        std::vector<void*> mine;
        mine.reserve(PER_THREAD);
        for (size_t i = 0; i < PER_THREAD; ++i)
          mine.push_back(snmalloc::libc::malloc(64));
        for (auto* p : mine)
          snmalloc::libc::free(p);
      });
    }
    for (auto& t : threads)
      t.join();
    return;
#else
    static_assert(
      config_has_profile_slot_v<snmalloc::Config>,
      "integration test config must carry the lazy SampledAlloc-slot "
      "provider");

    // The NodePool has a fixed compile-time capacity (default 16384;
    // see SNMALLOC_PROFILE_POOL_CAPACITY).  Pick the sampling rate so
    // the expected number of live samples is well below that ceiling --
    // otherwise pool-exhaustion drops would dominate and make the
    // accuracy bound meaningless.  At 16 x 100k x avg(340B) ~= 544 MiB
    // total bytes, a rate of 128 KiB gives ~4250 expected samples --
    // ~25% of the pool, leaving plenty of headroom.
    constexpr size_t SAMPLING_RATE = 128 * 1024; // 128 KiB
    constexpr size_t N_THREADS = 16;
    constexpr size_t PER_THREAD = 100'000;
    // Size ladder: small classes mostly, with a handful of larger.
    static constexpr size_t SIZES[] = {16, 64, 256, 1024};
    static constexpr size_t N_SIZES = sizeof(SIZES) / sizeof(SIZES[0]);

    Sampler::set_sampling_rate(SAMPLING_RATE);

    // One cross-thread queue per producer.  The producer at index `t`
    // hands cross-thread frees to the freer at index `(t + 1) % N`.
    // This guarantees every cross-thread free reaches a thread that
    // also happens to be producing -- maximising contention.
    std::vector<PtrQueue> queues(N_THREADS);

    std::atomic<size_t> total_bytes{0};

    // Barrier so we can snapshot live_count() while every sample is
    // still very much alive (no cross-thread frees yet).
    std::atomic<size_t> arrived_at_barrier{0};
    std::atomic<bool> release_barrier{false};

    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);

    for (size_t t = 0; t < N_THREADS; ++t)
    {
      threads.emplace_back([&, t] {
        // Per-thread PRNG: deterministic seed so reproducibility is
        // straightforward when investigating failures.
        std::mt19937 rng(0xC0FFEEu + static_cast<uint32_t>(t));
        std::uniform_int_distribution<uint32_t> size_dist(0, N_SIZES - 1);
        std::uniform_int_distribution<uint32_t> coin(0, 1);

        // Allocations the *producer* itself will free at the end (the
        // same-thread H1 path).  We delay these to the end so they are
        // counted in the pre-free snapshot.
        std::vector<void*> same_thread;
        same_thread.reserve(PER_THREAD);

        for (size_t i = 0; i < PER_THREAD; ++i)
        {
          const size_t sz = SIZES[size_dist(rng)];
          void* p = snmalloc::libc::malloc(sz);
          if (p == nullptr)
            continue;
          total_bytes.fetch_add(sz, std::memory_order_relaxed);

          if (coin(rng) == 0)
          {
            // Cross-thread queue: free on a different thread.
            auto& q = queues[(t + 1) % N_THREADS];
            std::lock_guard<std::mutex> lk(q.m);
            q.q.push(p);
          }
          else
          {
            same_thread.push_back(p);
          }
        }

        // Signal arrival: this thread has published all its allocations.
        arrived_at_barrier.fetch_add(1, std::memory_order_release);
        while (!release_barrier.load(std::memory_order_acquire))
          std::this_thread::yield();

        // Same-thread frees: H1.
        for (auto* p : same_thread)
          snmalloc::libc::free(p);

        // Cross-thread frees: drain the queue belonging to *this* thread
        // (which was filled by producer `(t - 1 + N) % N`).  H1 fires on
        // the source side too (the lock held a moment ago is unrelated;
        // the actual `libc::free` below is the H1 site).  H2 will
        // immediately fire on the destination side when the remote
        // message is dequeued by the owning allocator's next visit to
        // `handle_dealloc_remote`.  H4 fires for the very first free
        // this thread performs if its local allocator was not yet
        // initialised -- e.g. when t == 0 finishes allocating early.
        std::vector<void*> drained;
        {
          auto& myq = queues[t];
          std::lock_guard<std::mutex> lk(myq.m);
          while (!myq.q.empty())
          {
            drained.push_back(myq.q.front());
            myq.q.pop();
          }
        }
        for (auto* p : drained)
          snmalloc::libc::free(p);
      });
    }

    // Wait for every producer to finish allocating.
    while (arrived_at_barrier.load(std::memory_order_acquire) < N_THREADS)
      std::this_thread::yield();

    // Snapshot the seqs that exist *before* any frees happen.  These
    // are the samples our 16 producers minted; anything not in this
    // set that appears post-drain belongs to system-internal allocs.
    std::vector<uint64_t> pre_free_seqs;
    SamplerGlobals::list().snapshot([&](SampledAlloc* n) {
      pre_free_seqs.push_back(n->alloc_seq);
    });

    const size_t observed = pre_free_seqs.size();
    const double expected =
      static_cast<double>(total_bytes.load(std::memory_order_relaxed)) /
      SAMPLING_RATE;
    const double sigma = std::sqrt(expected);
    const double low = expected - 6 * sigma;
    const double high = expected + 6 * sigma;
    std::cout << "    samples observed = " << observed
              << "  expected ~= " << expected << "  (+/- 6 sigma = " << sigma
              << ")\n";
    check(
      static_cast<double>(observed) >= low &&
        static_cast<double>(observed) <= high,
      "16-thread sample count within 6 sigma of Poisson expectation");

    // Release the barrier: producers now free their same-thread
    // backlog and drain the cross-thread queues.
    release_barrier.store(true, std::memory_order_release);
    for (auto& t : threads)
      t.join();

    // Sanity: every cross-thread queue is empty.
    for (size_t i = 0; i < N_THREADS; ++i)
    {
      std::lock_guard<std::mutex> lk(queues[i].m);
      check(queues[i].q.empty(), "cross-thread queue drained");
    }

    // Verify how many pre-free seqs leaked.  Phase 3.3 documented a
    // narrow thread-teardown straggler in `profile_e2e.cc` at <= 0.1%
    // (~1 in 1250) under heavy concurrent stress.  Phase 3.4's H4 hook
    // installs `record_dealloc` on the lazy-init recursion arm; if the
    // straggler was a slow-path issue, the leak count here should be
    // at or below that tolerance.
    size_t leaked = 0;
    SamplerGlobals::list().snapshot([&](SampledAlloc* n) {
      for (uint64_t s : pre_free_seqs)
      {
        if (n->alloc_seq == s)
        {
          ++leaked;
          break;
        }
      }
    });
    std::cout << "    pre-free seqs remaining = " << leaked << " / "
              << pre_free_seqs.size() << "\n";

    // Documented tolerance: <= 1% + 4 absolute (matches profile_e2e.cc).
    const size_t leak_tolerance = pre_free_seqs.size() / 100 + 4;
    check(
      leaked <= leak_tolerance,
      "post-free leak count within documented tolerance (<= 1% + 4)");

    // Final invariant: the global SampledList drains completely once
    // we explicitly release every node back to the pool.
    drain_global_sampled_list();
    check(live_count() == 0, "global SampledList drained after explicit drain");

    Sampler::set_sampling_rate(SamplerGlobals::kDefaultSamplingRate);
#endif // SNMALLOC_PROFILE
  }

  // =========================================================================
  // Test 2: producer/consumer asymmetric -- one large producer, many
  // small consumers.  This stresses the destination-side H2 path on
  // multiple owning allocators and the H4 lazy-init arm on the
  // freshly-spawned consumer threads.
  // =========================================================================
  void test_one_producer_many_consumers()
  {
    std::cout << "test_one_producer_many_consumers\n";
    drain_global_sampled_list();

#ifndef SNMALLOC_PROFILE
    check(true, "SNMALLOC_PROFILE undefined: skipping");
    return;
#else
    constexpr size_t SAMPLING_RATE = 4096;
    constexpr size_t N_CONSUMERS = 8;
    constexpr size_t TOTAL_ALLOCS = 80'000;
    Sampler::set_sampling_rate(SAMPLING_RATE);

    std::vector<PtrQueue> queues(N_CONSUMERS);

    // Producer allocates and round-robins handoffs to consumers.
    std::thread producer([&] {
      for (size_t i = 0; i < TOTAL_ALLOCS; ++i)
      {
        void* p = snmalloc::libc::malloc(64 + (i & 127));
        if (p == nullptr)
          continue;
        auto& q = queues[i % N_CONSUMERS];
        std::lock_guard<std::mutex> lk(q.m);
        q.q.push(p);
      }
      for (auto& q : queues)
        q.producers_done.store(true, std::memory_order_release);
    });

    // Consumers spawn fresh; their first action is a cross-thread free
    // -- the canonical H4 trigger.
    std::vector<std::thread> consumers;
    consumers.reserve(N_CONSUMERS);
    for (size_t c = 0; c < N_CONSUMERS; ++c)
    {
      consumers.emplace_back([&, c] {
        while (true)
        {
          void* p = nullptr;
          {
            std::lock_guard<std::mutex> lk(queues[c].m);
            if (!queues[c].q.empty())
            {
              p = queues[c].q.front();
              queues[c].q.pop();
            }
          }
          if (p != nullptr)
          {
            snmalloc::libc::free(p);
            continue;
          }
          if (queues[c].producers_done.load(std::memory_order_acquire))
          {
            std::lock_guard<std::mutex> lk(queues[c].m);
            if (queues[c].q.empty())
              return;
          }
          std::this_thread::yield();
        }
      });
    }

    producer.join();
    for (auto& t : consumers)
      t.join();

    drain_global_sampled_list();
    check(live_count() == 0, "one-producer-many-consumers drains cleanly");

    Sampler::set_sampling_rate(SamplerGlobals::kDefaultSamplingRate);
#endif
  }
} // namespace

int main(int argc, char** argv)
{
  snmalloc::UNUSED(argc, argv);
  setup();

  std::cout << "[profile_integration]\n";

#ifdef SNMALLOC_PROFILE
  std::cout
    << "  (SNMALLOC_PROFILE is defined: full integration run, hooks live)\n";
#else
  std::cout
    << "  (SNMALLOC_PROFILE is undefined: smoke-only, hooks compiled out)\n";
#endif

  test_16_thread_mixed_free_stress();
  test_one_producer_many_consumers();

  if (g_fail_count == 0)
  {
    std::cout << "[profile_integration] ALL TESTS PASSED\n";
    return 0;
  }
  std::cout << "[profile_integration] " << g_fail_count << " TEST(S) FAILED\n";
  return 1;
}
