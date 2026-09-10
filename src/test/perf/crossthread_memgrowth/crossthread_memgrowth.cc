/**
 * Regression test for cross-thread committed memory growth (issue #814).
 *
 * Issue #814 reported that in a game engine workload — where worker threads
 * allocate large chunks (512KB–16MB) of memory and a different thread later
 * frees them — snmalloc's committed memory grew unboundedly even though the
 * number of live allocations remained roughly constant.
 *
 * This test reproduces that access pattern.  A pool of worker threads each:
 *   1. Allocate a large chunk and touch it to ensure commitment.
 *   2. Send it to a random *different* worker's mailbox (non-blocking).
 *   3. Drain their own mailbox and free whatever other workers sent them.
 *
 * Because every deallocation is of memory originally allocated by a different
 * thread, snmalloc must efficiently reclaim cross-thread frees.  The per-worker
 * mailbox capacity is bounded, so the number of live allocations (and therefore
 * the expected committed footprint) is bounded too.
 *
 * The test samples snmalloc's committed memory once per second for the
 * configured duration, then compares the average committed memory in the
 * 2nd quarter of the run (after warm-up) against the 4th quarter (end of
 * run).  If committed memory grew by more than 1.5x, the test fails
 * (exit code 1) indicating a possible regression.  Otherwise it passes
 * (exit code 0).
 *
 *   Usage:
 *     crossthread_memgrowth
 *       [--workers   N]     # worker threads     (default: 8)
 *       [--duration  N]     # run time seconds   (default: 120)
 *       [--min-size  N]     # min alloc bytes    (default: 524288 = 512KB)
 *       [--max-size  N]     # max alloc bytes    (default: 16777216 = 16MB)
 *       [--queue-cap N]     # per-worker queue   (default: 16)
 */

#include "test/opt.h"
#include "test/setup.h"
#include "test/xoroshiro.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <snmalloc/snmalloc.h>
#include <memory>
#include <thread>
#include <vector>

using namespace snmalloc;

// ──────────────────────── Per-worker mailbox ────────────────────────

/// An allocation in flight between workers.
struct Allocation
{
  void* ptr;
  size_t size;
};

/**
 * A bounded MPSC mailbox.  Other workers push allocations in; the owning
 * worker pops and frees them.  push() may block when the mailbox is full
 * (back-pressure keeps live allocation count bounded).
 */
class Mailbox
{
  std::mutex mu;
  std::condition_variable cv_not_full;
  std::condition_variable cv_not_empty;
  std::queue<Allocation> q;
  size_t cap;
  bool done{false};

public:
  explicit Mailbox(size_t capacity = 16) : cap(capacity) {}

  // Blocking push.  Returns false if the mailbox has been shut down.
  bool push(Allocation a)
  {
    std::unique_lock lock(mu);
    cv_not_full.wait(lock, [&] { return q.size() < cap || done; });
    if (done)
      return false;
    q.push(a);
    cv_not_empty.notify_one();
    return true;
  }

  // Non-blocking push.  Returns true if the item was enqueued.
  bool try_push(Allocation a)
  {
    std::lock_guard lock(mu);
    if (q.size() >= cap || done)
      return false;
    q.push(a);
    cv_not_empty.notify_one();
    return true;
  }

  // Non-blocking drain: move everything currently in the mailbox into `out`.
  // Returns the number of items drained.
  size_t drain(std::vector<Allocation>& out)
  {
    std::lock_guard lock(mu);
    size_t n = q.size();
    while (!q.empty())
    {
      out.push_back(q.front());
      q.pop();
    }
    cv_not_full.notify_all();
    return n;
  }

  // Blocking pop (used during final drain).
  bool pop(Allocation& out)
  {
    std::unique_lock lock(mu);
    cv_not_empty.wait(lock, [&] { return !q.empty() || done; });
    if (q.empty())
      return false;
    out = q.front();
    q.pop();
    cv_not_full.notify_one();
    return true;
  }

  void mark_done()
  {
    std::lock_guard lock(mu);
    done = true;
    cv_not_empty.notify_all();
    cv_not_full.notify_all();
  }

  size_t current_size()
  {
    std::lock_guard lock(mu);
    return q.size();
  }
};

// ──────────────────────── Measurement helpers ────────────────────────

/// A single point-in-time measurement taken once per second.
struct Sample
{
  size_t second;
  size_t allocs_total;
  size_t frees_total;
  size_t live_requested_bytes; // alloc'd - freed (client's view of live data)
  size_t committed_bytes;      // snmalloc's committed memory
  size_t peak_bytes;
};

static size_t get_committed()
{
  return Alloc::Config::Backend::get_current_usage();
}

static size_t get_peak()
{
  return Alloc::Config::Backend::get_peak_usage();
}

// ──────────────────────── Global state ────────────────────────

static std::atomic<bool> stop_flag{false};
static std::atomic<size_t> total_allocs{0};
static std::atomic<size_t> total_frees{0};
static std::atomic<size_t> total_allocated_bytes{0};
static std::atomic<size_t> total_freed_bytes{0};

/// Per-worker allocation statistics.  Each worker tracks the bytes it has
/// requested (via alloc) and the bytes it has returned (via dealloc) on
/// that thread, regardless of which thread originally allocated the memory.
struct alignas(64) WorkerStats
{
  std::atomic<size_t> requested_bytes{0}; // cumulative bytes alloc'd on this thread
  std::atomic<size_t> returned_bytes{0};  // cumulative bytes freed on this thread
};

/// Returns the net live requested bytes across all workers:
/// sum(alloc'd) - sum(freed).  This is the client's view of in-use memory
/// and represents the minimum the allocator must have committed.
static size_t
get_live_requested(std::vector<WorkerStats>& stats, size_t n_workers)
{
  size_t total_req = 0, total_ret = 0;
  for (size_t i = 0; i < n_workers; ++i)
  {
    total_req += stats[i].requested_bytes.load(std::memory_order_relaxed);
    total_ret += stats[i].returned_bytes.load(std::memory_order_relaxed);
  }
  return (total_req >= total_ret) ? (total_req - total_ret) : 0;
}

// ──────────────────────── Worker thread ────────────────────────

/**
 * Each worker:
 *  1. Allocates a large chunk.
 *  2. Sends it to a random OTHER worker's mailbox.
 *  3. Drains its own mailbox and frees whatever other workers sent it.
 *
 * This means every free() is of memory allocated by a different thread.
 */
void worker_thread(
  std::vector<std::unique_ptr<Mailbox>>& mailboxes,
  std::vector<WorkerStats>& stats,
  size_t n_workers,
  size_t min_size,
  size_t max_size,
  size_t id)
{
  xoroshiro::p128r32 rng(id + 7777, id * 31 + 1);
  size_t range = (max_size > min_size) ? (max_size - min_size) : 1;
  std::vector<Allocation> to_free;
  to_free.reserve(32);

  while (!stop_flag.load(std::memory_order_relaxed))
  {
    // --- Allocate ---
    size_t size = min_size + (rng.next() % range);
    void* ptr = snmalloc::alloc(size);
    if (ptr)
    {
      // Touch first and last pages to ensure commitment.
      reinterpret_cast<volatile char*>(ptr)[0] = 'A';
      if (size > 1)
        reinterpret_cast<volatile char*>(ptr)[size - 1] = 'Z';
    }
    stats[id].requested_bytes.fetch_add(size, std::memory_order_relaxed);
    total_allocated_bytes.fetch_add(size, std::memory_order_relaxed);
    total_allocs.fetch_add(1, std::memory_order_relaxed);

    // --- Try to send to a random OTHER worker (non-blocking) ---
    // Try several targets to avoid always falling back to local free.
    bool sent = false;
    for (size_t attempt = 0; attempt < 3; ++attempt)
    {
      size_t target = rng.next() % (n_workers - 1);
      if (target >= id)
        target++;
      if (mailboxes[target]->try_push({ptr, size}))
      {
        sent = true;
        break;
      }
    }
    if (!sent)
    {
      // All targets full — free the allocation ourselves to avoid deadlock.
      // This should be rare at steady state.
      snmalloc::dealloc(ptr);
      stats[id].returned_bytes.fetch_add(size, std::memory_order_relaxed);
      total_freed_bytes.fetch_add(size, std::memory_order_relaxed);
      total_frees.fetch_add(1, std::memory_order_relaxed);
    }

    // --- Drain own mailbox and free ---
    to_free.clear();
    mailboxes[id]->drain(to_free);
    for (auto& a : to_free)
    {
      snmalloc::dealloc(a.ptr);
      stats[id].returned_bytes.fetch_add(a.size, std::memory_order_relaxed);
      total_freed_bytes.fetch_add(a.size, std::memory_order_relaxed);
      total_frees.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // Final drain of own mailbox before exiting.
  to_free.clear();
  mailboxes[id]->drain(to_free);
  for (auto& a : to_free)
  {
    snmalloc::dealloc(a.ptr);
    stats[id].returned_bytes.fetch_add(a.size, std::memory_order_relaxed);
    total_freed_bytes.fetch_add(a.size, std::memory_order_relaxed);
    total_frees.fetch_add(1, std::memory_order_relaxed);
  }
}

// ──────────────────────── Main ────────────────────────

int main(int argc, char** argv)
{
  setup();

  opt::Opt o(argc, argv);
  size_t n_workers = o.is<size_t>("--workers", 8);
  size_t duration_s = o.is<size_t>("--duration", 120);
  size_t min_size = o.is<size_t>("--min-size", 512 * 1024);       // 512 KB
  size_t max_size = o.is<size_t>("--max-size", 16 * 1024 * 1024); // 16 MB
  size_t queue_cap = o.is<size_t>("--queue-cap", 16);

  if (n_workers < 2)
  {
    std::cerr << "Need at least 2 workers for cross-thread traffic.\n";
    return 1;
  }

  std::cout << "crossthread_memgrowth benchmark (issue #814)\n"
            << "  workers         = " << n_workers << "\n"
            << "  duration        = " << duration_s << " s\n"
            << "  size range      = " << min_size << " – " << max_size << "\n"
            << "  per-worker queue= " << queue_cap << "\n"
            << std::endl;

  // Create per-worker mailboxes.
  std::vector<std::unique_ptr<Mailbox>> mailboxes;
  mailboxes.reserve(n_workers);
  for (size_t i = 0; i < n_workers; ++i)
    mailboxes.push_back(std::make_unique<Mailbox>(queue_cap));

  // Per-worker allocation tracking.
  std::vector<WorkerStats> worker_stats(n_workers);

  std::vector<Sample> samples;
  samples.reserve(duration_s + 2);

  // Record baseline.
  samples.push_back({0, 0, 0, 0, get_committed(), get_peak()});

  // --- Launch workers ---
  std::vector<std::thread> workers;
  workers.reserve(n_workers);
  for (size_t i = 0; i < n_workers; ++i)
    workers.emplace_back(
      worker_thread,
      std::ref(mailboxes),
      std::ref(worker_stats),
      n_workers,
      min_size,
      max_size,
      i);

  // --- Sample committed memory once per second for the test duration ---
  for (size_t r = 1; r <= duration_s; ++r)
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    samples.push_back(
      {r,
       total_allocs.load(std::memory_order_relaxed),
       total_frees.load(std::memory_order_relaxed),
       get_live_requested(worker_stats, n_workers),
       get_committed(),
       get_peak()});
    // auto s = samples.back();
    // snmalloc::message<128>("Sample at {}s: allocs={}, frees={}, committed={} bytes, peak={} bytes",
    //   s.second,
    //   s.allocs_total,
    //   s.frees_total,
    //   s.committed_bytes,
    //   s.peak_bytes);
  }

  // --- Shut down workers and drain remaining allocations ---
  stop_flag.store(true, std::memory_order_relaxed);
  for (auto& mb : mailboxes)
    mb->mark_done();

  for (auto& t : workers)
    t.join();

  // Drain any remaining items in all mailboxes.
  {
    std::vector<Allocation> leftover;
    for (auto& mb : mailboxes)
    {
      Allocation a;
      while (mb->pop(a))
        leftover.push_back(a);
    }
    for (auto& a : leftover)
    {
      snmalloc::dealloc(a.ptr);
      total_freed_bytes.fetch_add(a.size, std::memory_order_relaxed);
      total_frees.fetch_add(1, std::memory_order_relaxed);
    }
  }

  // Final sample.
  samples.push_back(
    {duration_s + 1,
     total_allocs.load(),
     total_frees.load(),
     get_live_requested(worker_stats, n_workers),
     get_committed(),
     get_peak()});

  // ──────────── Report ────────────

  auto to_mb = [](size_t bytes) -> double {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
  };

  std::cout << std::fixed << std::setprecision(2);
  std::cout << std::setw(6) << "Time" << std::setw(12) << "Allocs"
            << std::setw(12) << "Frees" << std::setw(12) << "Live(MB)"
            << std::setw(16) << "Committed(MB)"
            << std::setw(12) << "Peak(MB)" << "\n";
  std::cout << std::string(70, '-') << "\n";

  for (const auto& s : samples)
  {
    std::cout << std::setw(6) << s.second << std::setw(12) << s.allocs_total
              << std::setw(12) << s.frees_total << std::setw(12)
              << to_mb(s.live_requested_bytes) << std::setw(16)
              << to_mb(s.committed_bytes) << std::setw(12)
              << to_mb(s.peak_bytes) << "\n";
  }

  std::cout << "\nSummary:\n"
            << "  Total allocs        : " << total_allocs.load() << "\n"
            << "  Total frees         : " << total_frees.load() << "\n"
            << "  Total alloc'd bytes : " << to_mb(total_allocated_bytes.load())
            << " MB\n"
            << "  Total freed bytes   : " << to_mb(total_freed_bytes.load())
            << " MB\n"
            << "  Final committed     : " << to_mb(get_committed()) << " MB\n"
            << "  Peak committed      : " << to_mb(get_peak()) << " MB\n";

  // ──────────── Growth analysis ────────────
  //
  // Compare average committed memory in the 2nd quarter (after warm-up)
  // against the 4th quarter (end of run).  Skipping the 1st quarter avoids
  // counting the initial ramp-up.  If the ratio exceeds 1.5, committed
  // memory is growing significantly over time — flag this as a regression.
  int exit_code = 0;

  if (samples.size() >= 8)
  {
    size_t n = samples.size() - 1; // exclude final (post-drain) sample
    size_t q2_lo = n / 4, q2_hi = n / 2;
    size_t q4_lo = 3 * n / 4, q4_hi = n - 1;

    double q2_avg = 0, q4_avg = 0;
    for (size_t i = q2_lo; i <= q2_hi; ++i)
      q2_avg += static_cast<double>(samples[i].committed_bytes);
    q2_avg /= static_cast<double>(q2_hi - q2_lo + 1);

    for (size_t i = q4_lo; i <= q4_hi; ++i)
      q4_avg += static_cast<double>(samples[i].committed_bytes);
    q4_avg /= static_cast<double>(q4_hi - q4_lo + 1);

    double growth = (q2_avg > 0) ? (q4_avg / q2_avg) : 0.0;

    std::cout << "\n  Avg committed (2nd quarter)   : "
              << to_mb(static_cast<size_t>(q2_avg)) << " MB\n"
              << "  Avg committed (4th quarter)   : "
              << to_mb(static_cast<size_t>(q4_avg)) << " MB\n"
              << "  Growth ratio (Q4/Q2)          : " << growth << "\n";

    if (growth > 1.5)
    {
      std::cout << "  FAIL: committed memory grew " << growth
                << "x over the run, possible unbounded growth.\n";
      exit_code = 1;
    }
    else
    {
      std::cout << "  PASS: committed memory appears stable.\n";
    }
  }

#ifndef NDEBUG
  debug_check_empty<snmalloc::Alloc::Config>();
#endif

  return exit_code;
}
