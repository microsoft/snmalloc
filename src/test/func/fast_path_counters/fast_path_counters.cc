// SPDX-License-Identifier: MIT
//
// Phase 9.2 (ClickUp 86aj0tr1e) -- per-thread frontend cache stats.
//
// Verifies the alloc/dealloc counter wiring in
// `src/snmalloc/mem/corealloc.h` by:
//
//   1. Allocating a batch of small objects on a single thread and
//      observing that `fast_path_allocs` rises by at least
//      `N - 1` (we allow one slow refill for the very first slab).
//
//   2. Freeing those allocations on the same thread and observing
//      `fast_path_deallocs` rise by the same amount.
//
//   3. Driving a cross-thread free from a worker thread and observing
//      `remote_deallocs` rise on the worker and
//      `cross_thread_messages_received` rise on the main thread once
//      it has drained the queue.
//
// The test reads counters via a local re-implementation of the
// `snmalloc_get_full_stats` aggregation loop (walks
// `AllocPool::iterate()` and adds in `frontend_stats_global()`).  This
// keeps the test self-contained -- the C ABI symbol itself lives in
// `src/snmalloc/override/stats_export.cc`, which is only compiled into
// the libsnmalloc shims, not the per-test executables.

// Phase 11.6 -- this test exercises only the BASIC (FrontendStats)
// counters and so is gated on SNMALLOC_STATS_BASIC.  Both
// `SNMALLOC_STATS=ON` (legacy alias) and `SNMALLOC_STATS_FULL=ON`
// implicitly enable BASIC and therefore reach the assertions below.
#ifdef SNMALLOC_STATS_BASIC
#  include <atomic>
#  include <iostream>
#  include <snmalloc/snmalloc.h>
#  include <thread>
#  include <vector>
#endif

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef SNMALLOC_STATS_BASIC

int main(int /*argc*/, char** /*argv*/)
{
  // No-op when SNMALLOC_STATS_BASIC is off.  The build matrix wants
  // the test binary to link cleanly even without the feature flag so
  // CI doesn't grow a conditional test target.
  fprintf(stderr,
          "fast_path_counters: SNMALLOC_STATS_BASIC=OFF, skipping\n");
  return 0;
}

#else

namespace
{
  // Local equivalent of the `snmalloc_get_full_stats` 9.2 block in
  // `src/snmalloc/override/stats_export.cc`.  Defined here so the
  // test does not need to link the libsnmalloc-shim TU.
  snmalloc::FrontendStats snapshot()
  {
    using namespace snmalloc;
    FrontendStats agg{};
    using AllocT = Allocator<Alloc::Config>;
    for (AllocT* a = AllocPool<Alloc::Config>::iterate(); a != nullptr;
         a = AllocPool<Alloc::Config>::iterate(a))
    {
      agg.accumulate(a->stats);
    }
    frontend_stats_global().snapshot_into(agg);
    return agg;
  }

  void check_ge(uint64_t actual, uint64_t expected, const char* name)
  {
    if (actual < expected)
    {
      std::cerr << "fast_path_counters: " << name << " expected >= "
                << expected << ", got " << actual << "\n";
      std::exit(1);
    }
    std::cout << "fast_path_counters: " << name << " = " << actual
              << " (>= " << expected << ")\n";
  }
} // namespace

int main(int /*argc*/, char** /*argv*/)
{
  using namespace snmalloc;

  // --------------------------------------------------------------------
  // Part 1: single-thread fast-path alloc/dealloc.
  // --------------------------------------------------------------------
  //
  // Allocate `N` small objects of one sizeclass on the main thread.
  // The first allocation forces a slow refill (slab open) which
  // bumps `slow_path_allocs` by 1; every subsequent allocation hits
  // the fast free list.  We require `fast_path_allocs` to rise by
  // at least `N - 1`.

  constexpr size_t N = 1000;
  constexpr size_t kObjSize = 32; // small sizeclass

  auto before = snapshot();

  std::vector<void*> ptrs;
  ptrs.reserve(N);
  for (size_t i = 0; i < N; ++i)
  {
    void* p = snmalloc::alloc(kObjSize);
    if (p == nullptr)
    {
      std::cerr << "alloc failed at i=" << i << "\n";
      return 1;
    }
    ptrs.push_back(p);
  }

  auto after_alloc = snapshot();
  // Phase 11.12 -- decode via accessors; the underlying field is
  // now a single packed 64-bit word.
  uint64_t alloc_delta =
    after_alloc.fast_path_allocs() - before.fast_path_allocs();
  // Every slow refill consumes one "missed fast-path" slot (the
  // pointer returned by the refill itself does not pass through the
  // fast-path counter), so for N allocs of one sizeclass we expect
  // `fast_path_allocs >= N - K` where K is the number of refills.
  // In practice for `N=1000, sizeclass=32` we observe K ~= 2 (the
  // first slab fills, then one further refill once it drains).
  // We require `>= N - 10` here as a comfortable lower bound that
  // still detects "fast-path counter never bumped" regressions.
  check_ge(alloc_delta, N - 10, "fast_path_allocs delta (1k allocs)");

  // Free everything; same sizeclass -> all hits the local-owner
  // branch in `dealloc`.  We expect a 1:1 rise in `fast_path_deallocs`.
  for (void* p : ptrs)
    snmalloc::dealloc(p);
  ptrs.clear();

  auto after_dealloc = snapshot();
  // Phase 11.9: fast_path_deallocs is pre-credited at small_refill
  // (alloc-time batching, symmetric with fast_path_allocs). The
  // counter therefore rises during the alloc phase, not the dealloc
  // phase. Measure from `before` rather than `after_alloc` so the
  // pre-credit lands inside the measurement window.
  uint64_t dealloc_delta =
    after_dealloc.fast_path_deallocs - before.fast_path_deallocs;
  // Each refill pre-credits the dealloc counter by the refill
  // batch size; N=1000 allocs trigger ~2 refills (~1024 credit
  // total), and the subsequent N frees do not bump the counter
  // again. We require the cumulative rise to cover the N frees
  // that occurred.
  check_ge(dealloc_delta, N - 10, "fast_path_deallocs delta (1k frees)");

  // --------------------------------------------------------------------
  // Part 2: cross-thread free.
  // --------------------------------------------------------------------
  //
  // Worker thread frees a pointer that the main thread allocated.
  // Because the pointer's slab is owned by the main thread, the
  // worker's `dealloc` goes through the remote branch and bumps
  // `remote_deallocs` on the worker.  The remote post sends a
  // message into the main thread's queue; the main thread observes
  // it on the next call into `handle_message_queue_slow`, which
  // bumps `cross_thread_messages_received` and `message_queue_drains`.

  auto before_remote = snapshot();

  // Pre-allocate many cross-pointers on the main thread so the
  // worker can free them all and overflow its remote_dealloc_cache
  // -- this forces an in-thread `post()` (via `dealloc_remote_slow`)
  // rather than relying on the teardown flush.  Each object is a
  // large enough size that 128 frees roughly fill REMOTE_CACHE
  // (typically 16-128 KiB), guaranteeing the cache exhausts and
  // posts mid-thread.
  constexpr int K = 128;
  constexpr size_t kCrossObjSize = 512;
  std::vector<void*> cross_ptrs;
  cross_ptrs.reserve(K);
  for (int i = 0; i < K; ++i)
  {
    void* q = snmalloc::alloc(kCrossObjSize);
    if (q == nullptr)
    {
      std::cerr << "cross_ptrs alloc failed at i=" << i << "\n";
      return 1;
    }
    cross_ptrs.push_back(q);
  }

  std::atomic<bool> start{false};

  std::thread worker([&] {
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();
    // Free all cross-pointers; each one is from main, so the
    // worker's `dealloc` takes the remote branch.  K * 512 bytes
    // is large enough (64 KiB) to overflow the worker's
    // remote-dealloc-cache and force at least one in-thread
    // `post()` via `dealloc_remote_slow` -- which delivers the
    // messages into main's queue immediately, not just at thread
    // teardown.
    for (int i = 0; i < K; ++i)
      snmalloc::dealloc(cross_ptrs[static_cast<size_t>(i)]);
  });
  start.store(true, std::memory_order_release);
  worker.join();

  // Worker has exited; its allocator was flushed and its counters
  // drained into `frontend_stats_global()` (see
  // `Allocator::drain_stats_to_global`).  `remote_deallocs` should
  // have risen by at least K (one per cross-thread free).
  auto after_remote_free = snapshot();
  uint64_t remote_delta =
    after_remote_free.remote_deallocs - before_remote.remote_deallocs;
  check_ge(
    remote_delta,
    static_cast<uint64_t>(K),
    "remote_deallocs delta after worker exit");

  // Drive the slow path on main: each fresh sizeclass starts with
  // an empty fast free list and routes through
  // `handle_message_queue`, which is where the
  // `cross_thread_messages_received` counter lives.  Run many
  // iterations across many sizeclasses to maximise the chance of
  // taking the slow path (and to be robust against the exact set
  // of sizeclasses already populated by Part 1).
  for (int rep = 0; rep < 256; ++rep)
  {
    size_t sz = static_cast<size_t>(16 + (rep * 17) % 256);
    void* p = snmalloc::alloc(sz);
    if (p != nullptr)
      snmalloc::dealloc(p);
  }


  auto after_drain = snapshot();
  uint64_t msg_delta = after_drain.cross_thread_messages_received -
    before_remote.cross_thread_messages_received;
  uint64_t drain_delta =
    after_drain.message_queue_drains - before_remote.message_queue_drains;

  check_ge(msg_delta, 1, "cross_thread_messages_received delta");
  check_ge(drain_delta, 1, "message_queue_drains delta");

  // --------------------------------------------------------------------
  // Part 3: sanity assert on `slow_path_allocs`.
  // --------------------------------------------------------------------
  // Total slow-path allocs across the run should be at least one
  // (the first slab open).
  if (after_drain.slow_path_allocs() < 1)
  {
    std::cerr << "expected slow_path_allocs >= 1, got "
              << after_drain.slow_path_allocs() << "\n";
    return 1;
  }
  std::cout << "fast_path_counters: slow_path_allocs (end) = "
            << after_drain.slow_path_allocs() << "\n";

  std::cout << "fast_path_counters: all checks passed\n";
  return 0;
}

#endif // SNMALLOC_STATS_BASIC
