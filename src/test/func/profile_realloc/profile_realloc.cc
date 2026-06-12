// SPDX-License-Identifier: MIT
//
// Realloc event hook tests (ticket 86aj0hk9y).
//
// Exercises `snmalloc::profile::record_realloc`, the in-place realloc
// hook plumbed through `snmalloc::libc::realloc` at
// `src/snmalloc/global/libc.h`.
//
// Coverage:
//
//   1. Alloc, then in-place realloc to a new size that lands in the
//      SAME sizeclass.  Assert the persisted SampledList slot has its
//      `requested_size` updated to the new value (option C from the
//      ticket).  `allocated_size` is the sizeclass-rounded value and
//      stays the same since the sizeclass did not change.
//
//   2. Out-of-place realloc (target size in a DIFFERENT sizeclass).
//      The dealloc hook clears the original slot and the alloc hook
//      stashes a fresh sample for the returned pointer.  This is the
//      contract we keep on the slow path -- a new alloc-time event,
//      no synthesised Resize event.
//
//   3. Realloc on an UNSAMPLED allocation: nothing happens to the
//      SampledList (no spurious sample created on the resize).
//
//   4. Resize event broadcast: register an
//      AllocationSampleList handler and confirm in-place realloc
//      triggers a callback whose `kind == Resize` and whose
//      `requested_size` matches the post-resize value.
//
// When SNMALLOC_PROFILE is undefined the alloc/dealloc hooks are
// compile-time no-ops and the test degrades to a smoke run that
// just exercises the realloc shim.

#include <test/setup.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include <snmalloc/backend/globalconfig.h>
#include <snmalloc/snmalloc_core.h>

#include <snmalloc/profile/profile.h>
#include <snmalloc/profile/record.h>

namespace snmalloc
{
  // Profile-enabled Config: identical to profile_e2e / profile_streaming.
  using Config = snmalloc::StandardConfigClientMeta<
    LazyArrayClientMetaDataProvider<std::atomic<profile::SampledAlloc*>>>;
} // namespace snmalloc

#define SNMALLOC_PROVIDE_OWN_CONFIG
#include <snmalloc/snmalloc.h>

using snmalloc::profile::AllocationSampleList;
using snmalloc::profile::config_has_profile_slot_v;
using snmalloc::profile::SampledAlloc;
using snmalloc::profile::SampledAllocKind;
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

  // Note: there is no easy in-process way to force the per-thread
  // Sampler countdown to refresh once it has been parked at
  // INT64_MAX/2 (rate=0) or filled by a previous rate=2^62 draw --
  // the countdown only re-evaluates the global rate on slow-path
  // entry, and that requires consuming the existing counter.
  // Mitigation: order the tests so any test that bumps the rate up
  // runs LAST.  See main().

  // -----------------------------------------------------------------------
  // Test 1: in-place realloc updates the persisted slot's size fields.
  //
  // Strategy: sampler rate = 1 byte so every alloc is sampled.  Alloc
  // a small object, then realloc(p, original_requested + 1) to a new
  // requested size that still rounds to the same sizeclass.  The
  // persisted SampledAlloc node should then see `requested_size`
  // updated to the new value; `allocated_size` is unchanged because
  // the sizeclass is the same.
  // -----------------------------------------------------------------------
  void test_inplace_realloc_updates_slot()
  {
    std::cout << "test_inplace_realloc_updates_slot\n";
    drain_global_sampled_list();

#ifndef SNMALLOC_PROFILE
    void* p = snmalloc::libc::malloc(64);
    void* p2 = snmalloc::libc::realloc(p, 96);
    check(p2 != nullptr, "realloc returned non-null even with profile off");
    snmalloc::libc::free(p2);
    return;
#else
    // Force every allocation to be sampled by setting rate = 1 byte
    // (the Sampler treats any non-zero rate as a Poisson mean; rate=1
    // means a sample on essentially every alloc).
    Sampler::set_sampling_rate(1);

    // Warm-up alloc/free so the per-thread sampler countdown adopts
    // the new rate.
    {
      void* warm = snmalloc::libc::malloc(8);
      snmalloc::libc::free(warm);
    }
    drain_global_sampled_list();

    // 100 bytes rounds up to the 128-byte sizeclass on every snmalloc
    // configuration we care about, giving us ~28 bytes of slack to
    // grow into without crossing a sizeclass boundary.
    constexpr size_t OBJ_SIZE = 100;
    void* p = snmalloc::libc::malloc(OBJ_SIZE);

    // Find the SampledAlloc node by alloc_addr.  We can't reach into
    // find_profile_slot directly without leaking config-private types
    // here, but a snapshot scan is plenty for a test.
    SampledAlloc* matched = nullptr;
    size_t pre_requested = 0;
    SamplerGlobals::list().snapshot([&](SampledAlloc* n) {
      if (n->alloc_addr == reinterpret_cast<uintptr_t>(p))
      {
        matched = n;
        pre_requested = n->requested_size;
      }
    });
    if (matched == nullptr)
    {
      // With rate=1 the sample should always have fired.  Bail out
      // rather than dereferencing nullptr below.
      check(false, "alloc was sampled (matched != nullptr)");
      snmalloc::libc::free(p);
      drain_global_sampled_list();
      return;
    }
    check(matched != nullptr, "alloc was sampled");
    check(
      pre_requested == OBJ_SIZE, "pre-realloc requested_size == OBJ_SIZE");

    // Realloc to a slightly larger size that still rounds into the
    // SAME sizeclass.  alloc_size(p) gives us the sizeclass-rounded
    // size; we pick anything between OBJ_SIZE+1 and that as our new
    // requested size.
    const size_t allocated = snmalloc::alloc_size(p);
    const size_t new_requested =
      (allocated > OBJ_SIZE) ? (OBJ_SIZE + 1) : OBJ_SIZE;
    void* p2 = snmalloc::libc::realloc(p, new_requested);
    if (allocated > OBJ_SIZE)
    {
      // The new size fits in the same sizeclass -- realloc must
      // return the same pointer (the in-place fast path fired).
      check(p2 == p, "in-place realloc returned the same pointer");
    }
    else
    {
      // Degenerate case (e.g. minimum sizeclass): the fast path may
      // not fire.  Skip the rest of the test.
      std::cout << "    (sizeclass " << allocated
                << " has no slack above OBJ_SIZE; skipping rest)\n";
      snmalloc::libc::free(p2);
      drain_global_sampled_list();
      return;
    }

    // Re-walk the list and confirm the slot's requested_size has been
    // updated; allocated_size stays the same (same sizeclass).
    bool found_updated = false;
    SamplerGlobals::list().snapshot([&](SampledAlloc* n) {
      if (n->alloc_addr == reinterpret_cast<uintptr_t>(p2))
      {
        if (n->requested_size == new_requested)
          found_updated = true;
      }
    });
    check(
      found_updated,
      "in-place realloc updated the persisted requested_size in place");
    // After the in-place realloc the persisted allocated_size reflects
    // the sizeclass-rounded value passed by libc.h (`alloc_size(ptr)`,
    // i.e. the slab capacity).  The original alloc-time
    // `allocated_size` recorded by globalalloc.h is the aligned-but-
    // not-yet-sizeclass-rounded request size, which can differ from
    // the slab capacity; the realloc hook deliberately normalises both
    // fields to the post-realloc view since that is the size a
    // streaming consumer would expect to see for the resized object.
    check(
      matched->allocated_size == allocated,
      "in-place realloc set allocated_size to alloc_size(ptr)");
    check(
      matched->requested_size == new_requested,
      "in-place realloc set requested_size to the new caller-requested size");

    snmalloc::libc::free(p2);
    drain_global_sampled_list();
    Sampler::set_sampling_rate(SamplerGlobals::kDefaultSamplingRate);
#endif
  }

  // -----------------------------------------------------------------------
  // Test 2: out-of-place realloc (size change crosses sizeclass).  The
  // existing alloc/dealloc hooks already do the right thing; the
  // realloc hook does NOT fire.  We verify by checking that the new
  // pointer has a fresh sample (different alloc_seq) and the old
  // pointer's sample is gone.
  // -----------------------------------------------------------------------
  void test_outofplace_realloc_uses_alloc_dealloc()
  {
    std::cout << "test_outofplace_realloc_uses_alloc_dealloc\n";
    drain_global_sampled_list();

#ifndef SNMALLOC_PROFILE
    void* p = snmalloc::libc::malloc(64);
    void* p2 = snmalloc::libc::realloc(p, 4096);
    check(p2 != nullptr, "realloc to larger size returned non-null");
    snmalloc::libc::free(p2);
    return;
#else
    Sampler::set_sampling_rate(1);
    {
      void* warm = snmalloc::libc::malloc(8);
      snmalloc::libc::free(warm);
    }
    drain_global_sampled_list();

    void* p = snmalloc::libc::malloc(64);
    uint64_t pre_seq = 0;
    SamplerGlobals::list().snapshot([&](SampledAlloc* n) {
      if (n->alloc_addr == reinterpret_cast<uintptr_t>(p))
        pre_seq = n->alloc_seq;
    });
    check(pre_seq != 0, "original alloc was sampled");

    // Realloc to a substantially larger size -- guaranteed to cross
    // into a different sizeclass.
    void* p2 = snmalloc::libc::realloc(p, 8192);
    check(p2 != nullptr, "out-of-place realloc returned non-null");
    // Out-of-place: a real allocator typically returns a different
    // pointer.  We don't strictly require that (could in principle
    // be the same address if the original slab got immediately
    // recycled), but the alloc_seq MUST differ if a new sample fired.

    // The new pointer should have its own fresh sample.
    uint64_t post_seq = 0;
    SamplerGlobals::list().snapshot([&](SampledAlloc* n) {
      if (n->alloc_addr == reinterpret_cast<uintptr_t>(p2))
        post_seq = n->alloc_seq;
    });
    check(
      post_seq != 0 && post_seq != pre_seq,
      "out-of-place realloc produced a fresh sample for the new pointer");

    // The original sample's pre_seq must be gone (dealloc hook drained
    // it via the H1 path).
    bool original_remains = false;
    SamplerGlobals::list().snapshot([&](SampledAlloc* n) {
      if (n->alloc_seq == pre_seq)
        original_remains = true;
    });
    check(
      !original_remains,
      "out-of-place realloc cleared the original sample");

    snmalloc::libc::free(p2);
    drain_global_sampled_list();
    Sampler::set_sampling_rate(SamplerGlobals::kDefaultSamplingRate);
#endif
  }

  // -----------------------------------------------------------------------
  // Test 3: realloc on an UNSAMPLED allocation does not create a new
  // sample.  The hook short-circuits because the slot is null.
  // -----------------------------------------------------------------------
  void test_realloc_unsampled_alloc_is_noop()
  {
    std::cout << "test_realloc_unsampled_alloc_is_noop\n";
    drain_global_sampled_list();

#ifndef SNMALLOC_PROFILE
    void* p = snmalloc::libc::malloc(64);
    void* p2 = snmalloc::libc::realloc(p, 96);
    snmalloc::libc::free(p2);
    return;
#else
    // Sampling rate ~= 2^62 -> effectively no samples will fire.
    Sampler::set_sampling_rate(static_cast<size_t>(1) << 62);
    {
      // Warm-up so the per-thread countdown adopts the new rate.
      void* warm = snmalloc::libc::malloc(8);
      snmalloc::libc::free(warm);
    }
    drain_global_sampled_list();

    const size_t before = SamplerGlobals::list().debug_count();
    void* p = snmalloc::libc::malloc(64);
    void* p2 = snmalloc::libc::realloc(p, 96);
    const size_t after = SamplerGlobals::list().debug_count();

    check(
      after == before, "unsampled realloc produced zero new samples");

    snmalloc::libc::free(p2);
    drain_global_sampled_list();
    Sampler::set_sampling_rate(SamplerGlobals::kDefaultSamplingRate);
#endif
  }

  // -----------------------------------------------------------------------
  // Test 4: in-place realloc broadcasts a Resize event with the
  // post-resize sizes.  Registers a counting handler with the global
  // AllocationSampleList for the duration of the test.
  // -----------------------------------------------------------------------
  std::atomic<size_t> g_resize_count{0};
  std::atomic<size_t> g_alloc_count{0};
  std::atomic<size_t> g_last_resize_requested{0};
  std::atomic<size_t> g_last_resize_allocated{0};

  [[maybe_unused]] void
  resize_counting_callback(const SampledAlloc& s) noexcept
  {
    if (s.kind == static_cast<uint8_t>(SampledAllocKind::Resize))
    {
      g_resize_count.fetch_add(1, std::memory_order_relaxed);
      g_last_resize_requested.store(
        s.requested_size, std::memory_order_relaxed);
      g_last_resize_allocated.store(
        s.allocated_size, std::memory_order_relaxed);
    }
    else
    {
      g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void test_inplace_realloc_broadcasts_resize_event()
  {
    std::cout << "test_inplace_realloc_broadcasts_resize_event\n";
    drain_global_sampled_list();

#ifndef SNMALLOC_PROFILE
    check(
      true, "SNMALLOC_PROFILE undefined: skipping resize broadcast test");
    return;
#else
    g_resize_count.store(0, std::memory_order_relaxed);
    g_alloc_count.store(0, std::memory_order_relaxed);
    g_last_resize_requested.store(0, std::memory_order_relaxed);
    g_last_resize_allocated.store(0, std::memory_order_relaxed);

    Sampler::set_sampling_rate(1);
    {
      void* warm = snmalloc::libc::malloc(8);
      snmalloc::libc::free(warm);
    }
    drain_global_sampled_list();

    const int rc = AllocationSampleList::global().register_handler(
      resize_counting_callback);
    check(
      rc == AllocationSampleList::kOk,
      "AllocationSampleList::register_handler returned kOk");

    // 100 bytes rounds up to the 128-byte sizeclass on every snmalloc
    // configuration we care about, giving us ~28 bytes of slack to
    // grow into without crossing a sizeclass boundary.
    constexpr size_t OBJ_SIZE = 100;
    void* p = snmalloc::libc::malloc(OBJ_SIZE);
    const size_t allocated_before = snmalloc::alloc_size(p);

    // Snapshot the alloc-event count before the realloc so we can
    // distinguish the broadcast it triggers from any concurrent
    // alloc-event broadcasts that fired during the malloc above.
    const size_t resize_before =
      g_resize_count.load(std::memory_order_relaxed);

    if (allocated_before <= OBJ_SIZE)
    {
      // Minimum-sizeclass slab; no room to grow in place.  Skip.
      std::cout << "    (no slack in sizeclass; skipping resize event)\n";
      snmalloc::libc::free(p);
      (void)AllocationSampleList::global().unregister_handler(
        resize_counting_callback);
      drain_global_sampled_list();
      return;
    }

    const size_t new_requested = OBJ_SIZE + 1;
    void* p2 = snmalloc::libc::realloc(p, new_requested);
    check(p2 == p, "in-place realloc returned the same pointer");

    const size_t resize_after =
      g_resize_count.load(std::memory_order_relaxed);
    check(
      resize_after > resize_before,
      "in-place realloc fired at least one Resize broadcast event");

    const size_t obs_req =
      g_last_resize_requested.load(std::memory_order_relaxed);
    const size_t obs_alloc =
      g_last_resize_allocated.load(std::memory_order_relaxed);
    check(
      obs_req == new_requested,
      "Resize broadcast carried the post-resize requested_size");
    check(
      obs_alloc == allocated_before,
      "Resize broadcast carried the (unchanged) allocated_size");

    (void)AllocationSampleList::global().unregister_handler(
      resize_counting_callback);
    snmalloc::libc::free(p2);
    drain_global_sampled_list();
    Sampler::set_sampling_rate(SamplerGlobals::kDefaultSamplingRate);
#endif
  }
} // namespace

int main(int argc, char** argv)
{
  snmalloc::UNUSED(argc, argv);
  setup();

  std::cout << "[profile_realloc]\n";

#ifdef SNMALLOC_PROFILE
  std::cout << "  (SNMALLOC_PROFILE is defined: full realloc-hook run)\n";
#else
  std::cout << "  (SNMALLOC_PROFILE is undefined: smoke-test only)\n";
#endif

  // Test ordering: the unsampled test sets the global rate to ~2^62
  // and (under the current Sampler design) the per-thread countdown
  // does not refresh until the slow path is next entered.  To keep
  // subsequent rate=1 tests sampling reliably, run that test LAST.
  test_inplace_realloc_updates_slot();
  test_outofplace_realloc_uses_alloc_dealloc();
  test_inplace_realloc_broadcasts_resize_event();
  test_realloc_unsampled_alloc_is_noop();

  if (g_fail_count == 0)
  {
    std::cout << "[profile_realloc] ALL TESTS PASSED\n";
    return 0;
  }
  std::cout << "[profile_realloc] " << g_fail_count << " TEST(S) FAILED\n";
  return 1;
}
