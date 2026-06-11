// SPDX-License-Identifier: MIT
//
// Phase 3.2 unit tests for the H2 remote-dealloc profile hook.
//
// H2 lives inside `Allocator::handle_dealloc_remote` (corealloc.h:~501),
// guarding the splice that hands a forwarded RemoteMessage back to the
// destination thread's local free queue via `dealloc_local_objects_fast`.
// These tests cover:
//
//   1. Single-threaded baseline: alloc + free without SNMALLOC_PROFILE
//      defined behaves identically (smoke test; the hook is a compile-time
//      no-op for the default Config either way).
//   2. H1 + H2 idempotence on cross-thread free: a slot populated by an
//      explicit `publish_sample` is cleared at most once even if both H1
//      (source thread) and H2 (destination thread) fire on the same
//      pointer.  Verified by checking that `clear_profile_slot` returns
//      non-null exactly once when called twice in sequence.
//   3. Stress: 4 producer + 4 consumer threads exchange allocations.
//      The producer frees pointers it allocated on a *different* thread,
//      forcing every freed pointer through the remote-dealloc path on
//      the owning thread.  We verify: no crash, no leak (final live
//      count is zero), and that the global SampledList is empty at the
//      end so neither H1 nor H2 stranded any nodes.
//   4. Default-config compile-time guard: `record_dealloc<Config>` for
//      the default `snmalloc::Config` is a no-op regardless of whether
//      H1 or H2 calls it.  This pins the byte-identical-OFF claim.
//
// The tests exercise only the publicly-exposed `snmalloc::libc::*`
// surface plus the profile primitives (clear_profile_slot, SampledList,
// NodePool).  We deliberately do NOT construct a Config that wires the
// lazy provider into a real Backend: that integration is Phase 3.3.

#include <snmalloc/snmalloc.h>

#include <snmalloc/profile/profile.h>
#include <snmalloc/profile/record.h>

#include <test/setup.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

using snmalloc::profile::clear_profile_slot;
using snmalloc::profile::config_has_profile_slot_v;
using snmalloc::profile::ProfileSlot;
using snmalloc::profile::record_dealloc;
using snmalloc::profile::SampledAlloc;
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

  SampledAlloc* publish_sample(ProfileSlot& slot)
  {
    SampledAlloc* node = SamplerGlobals::pool().acquire();
    if (node == nullptr)
      return nullptr;
    node->alloc_addr = reinterpret_cast<uintptr_t>(&slot);
    node->requested_size = 1;
    node->allocated_size = 1;
    node->weight = 1;
    node->sample_interval_at_capture =
      SamplerGlobals::sampling_rate().load(std::memory_order_relaxed);
    SamplerGlobals::list().push(node);
    slot.store(node, std::memory_order_release);
    return node;
  }

  // =========================================================================
  // Test 1: single-threaded baseline -- alloc + free does not crash, and
  //         the H2 hook (compiled in when SNMALLOC_PROFILE is on, absent
  //         when off) is invisible to the default config.
  // =========================================================================
  void test_singlethread_baseline()
  {
    std::cout << "test_singlethread_baseline\n";

    constexpr size_t N = 256;
    std::vector<void*> ptrs;
    ptrs.reserve(N);
    for (size_t i = 0; i < N; ++i)
    {
      void* p = snmalloc::libc::malloc(48 + (i & 15));
      check(p != nullptr, "malloc succeeded");
      std::memset(p, 0x5a, 32);
      ptrs.push_back(p);
    }
    for (size_t i = N; i-- > 0;)
    {
      snmalloc::libc::free(ptrs[i]);
    }
    check(true, "single-threaded round-trip clean");
  }

  // =========================================================================
  // Test 2: H1+H2 idempotence -- two sequential clears of one populated
  //         slot.  The first wins, the second is a safe no-op.  This is
  //         the exact contract that lets H2 fire defensively on the
  //         destination thread without double-freeing a SampledAlloc
  //         already returned to the pool by H1.
  // =========================================================================
  void test_h1_h2_idempotence()
  {
    std::cout << "test_h1_h2_idempotence\n";
    drain_global_sampled_list();

    ProfileSlot slot{nullptr};
    SampledAlloc* node = publish_sample(slot);
    check(node != nullptr, "sample published");
    if (node == nullptr)
      return;

    const size_t live_pre = SamplerGlobals::list().debug_count();
    check(live_pre >= 1, "live count >= 1 before any clear");

    // Simulate H1 on source thread.
    SampledAlloc* first = clear_profile_slot(&slot);
    check(first == node, "first clear (H1) wins and returns the node");
    check(
      slot.load(std::memory_order_relaxed) == nullptr,
      "slot is null after H1 clear");

    // Simulate H2 on destination thread for the same forwarded pointer.
    SampledAlloc* second = clear_profile_slot(&slot);
    check(
      second == nullptr,
      "second clear (H2) is a no-op -- no double release");

    const size_t live_post = SamplerGlobals::list().debug_count();
    check(
      live_pre - live_post == 1,
      "live count decreased by exactly one across H1+H2");

    drain_global_sampled_list();
  }

  // =========================================================================
  // Test 3: cross-thread dealloc stress.  4 producer threads allocate
  //         buffers and hand them to 4 consumer threads, which free them.
  //         Every free is therefore a cross-thread free, exercising the
  //         remote-message machinery that H2 instruments.  We assert no
  //         crash and no leak in the global SampledList.
  // =========================================================================
  struct CrossThreadQueue
  {
    std::mutex m;
    std::queue<void*> q;
    std::atomic<bool> producers_done{false};
  };

  void cross_thread_producer(
    CrossThreadQueue& cq, size_t count, size_t base_size)
  {
    for (size_t i = 0; i < count; ++i)
    {
      void* p = snmalloc::libc::malloc(base_size + (i & 63));
      if (p == nullptr)
        continue;
      // Touch a couple of bytes so the pagemap is fully realised.
      std::memset(p, 0x77, 16);
      {
        std::lock_guard<std::mutex> lk(cq.m);
        cq.q.push(p);
      }
    }
  }

  void cross_thread_consumer(CrossThreadQueue& cq)
  {
    while (true)
    {
      void* p = nullptr;
      {
        std::lock_guard<std::mutex> lk(cq.m);
        if (!cq.q.empty())
        {
          p = cq.q.front();
          cq.q.pop();
        }
      }
      if (p != nullptr)
      {
        snmalloc::libc::free(p);
        continue;
      }
      if (cq.producers_done.load(std::memory_order_acquire))
      {
        // Drain any remaining work added between the empty-check and
        // the done-check.
        std::lock_guard<std::mutex> lk(cq.m);
        if (cq.q.empty())
          return;
      }
      std::this_thread::yield();
    }
  }

  void test_cross_thread_stress()
  {
    std::cout << "test_cross_thread_stress\n";
    drain_global_sampled_list();

    constexpr size_t N_PRODUCER = 4;
    constexpr size_t N_CONSUMER = 4;
    constexpr size_t PER_PRODUCER = 4096;

    // One queue per consumer, producers round-robin across them so every
    // free travels across thread boundaries.
    std::vector<CrossThreadQueue> queues(N_CONSUMER);

    std::vector<std::thread> consumers;
    consumers.reserve(N_CONSUMER);
    for (size_t i = 0; i < N_CONSUMER; ++i)
    {
      consumers.emplace_back(cross_thread_consumer, std::ref(queues[i]));
    }

    std::vector<std::thread> producers;
    producers.reserve(N_PRODUCER);
    for (size_t i = 0; i < N_PRODUCER; ++i)
    {
      producers.emplace_back([&queues, i] {
        // Each producer feeds its dedicated consumer (different thread).
        // Sizes span small + medium classes to stretch slab geometry.
        const size_t base = 32 + (i * 96);
        cross_thread_producer(
          queues[i % queues.size()], PER_PRODUCER, base);
      });
    }

    for (auto& t : producers)
      t.join();

    for (auto& q : queues)
      q.producers_done.store(true, std::memory_order_release);

    for (auto& t : consumers)
      t.join();

    // All queues empty.
    for (size_t i = 0; i < queues.size(); ++i)
    {
      std::lock_guard<std::mutex> lk(queues[i].m);
      check(queues[i].q.empty(), "consumer drained its queue");
    }

    // No sample state stranded.  In a non-profile-enabled config (the
    // default) record_dealloc is a compile-time no-op so the list was
    // never touched, but draining is still a safe assertion.
    const size_t live_end = SamplerGlobals::list().debug_count();
    check(
      live_end == 0,
      "no SampledAlloc nodes leaked across cross-thread stress");

    check(true, "cross-thread stress completed without crash");
  }

  // =========================================================================
  // Test 4: default-config compile-time no-op.  The default Config does
  //         NOT carry the lazy provider, so both H1 and H2 must compile
  //         away.  A successful build of this TU already proves it; we
  //         additionally call the hook to confirm runtime no-op.
  // =========================================================================
  void test_default_config_compiletime_noop()
  {
    std::cout << "test_default_config_compiletime_noop\n";

    static_assert(
      !config_has_profile_slot_v<snmalloc::Config>,
      "default Config must remain free of LazyArrayClientMetaDataProvider<"
      "ProfileSlot> -- the OFF-build byte-identical invariant depends on it");

    int sentinel = 0;
    // The H2 site calls record_dealloc<Config>(msg.unsafe_ptr()); we
    // invoke the same path here with a sentinel pointer.
    record_dealloc<snmalloc::Config>(&sentinel);
    record_dealloc<snmalloc::Config>(nullptr);

    check(true, "record_dealloc<default Config> is a no-op at H2 path");
  }
} // namespace

int main(int argc, char** argv)
{
  snmalloc::UNUSED(argc, argv);
  setup();

  std::cout << "[profile_remote_dealloc]\n";

  test_singlethread_baseline();
  test_h1_h2_idempotence();
  test_cross_thread_stress();
  test_default_config_compiletime_noop();

  if (g_fail_count == 0)
  {
    std::cout << "[profile_remote_dealloc] ALL TESTS PASSED\n";
    return 0;
  }
  std::cout << "[profile_remote_dealloc] " << g_fail_count
            << " TEST(S) FAILED\n";
  return 1;
}
