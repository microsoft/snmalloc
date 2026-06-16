// SPDX-License-Identifier: MIT
//
// Phase 3.4 unit tests for the H3 + H4 dealloc edge-case profile hooks.
//
// H3 lives inside `Allocator::dealloc_remote` (corealloc.h, the
// SecondaryAllocator escape arm).  It catches pointers whose pagemap
// entry reports `!is_owned()` -- typically GWP-ASan guard pages, a
// sandboxed SecondaryAllocator's pool, or other non-snmalloc memory
// that snmalloc is being asked to free on behalf of the platform.
//
// H4 lives inside the lazy-init lambda of
// `Allocator::dealloc_remote_slow` (corealloc.h).  When `check_init`
// has to acquire an allocator before the free can proceed, the
// acquired allocator may itself be the originating allocator -- so
// the design re-enters `Allocator::dealloc(p)` from the top.  H4
// fires immediately before that recursive call to keep the
// recursion-guard pair complete.
//
// Both sites are extreme edge cases of `Allocator::dealloc`; an
// ordinary same-thread or remote-thread free never visits either.
// Direct triggering from portable user code is therefore neither
// possible nor desirable; this TU instead validates the *contract*
// that every dealloc hook depends on:
//
//   1. Idempotence -- multiple sequential `clear_profile_slot` calls
//      on the same slot return non-null exactly once.  H1+H2+H3+H4
//      can all fire on the same pointer (H1 always, H3 only on the
//      SecondaryAllocator branch, H4 only on the lazy-init
//      recursion); the CAS in `clear_profile_slot` guarantees only
//      one of them publishes a release.
//
//   2. Triple- and quadruple-clear safety -- if the (purely
//      hypothetical) future code path lets H1, H3, and the
//      H4-driven recursive H1 all run on a single pointer, the
//      sampled-list and node-pool invariants survive.
//
//   3. nullptr robustness -- the H3 hook is gated by p_tame != null
//      in the existing code, but `record_dealloc` itself is also
//      nullptr-safe (early-return).  We confirm that contract here
//      since H3 *is* reached for non-snmalloc-owned non-null
//      pointers.
//
//   4. Default-config compile-time no-op -- both H3 and H4 must
//      compile to literally nothing for `snmalloc::Config`, the
//      default that does not carry the lazy provider.
//
// The tests use only the publicly-exposed primitives in
// `snmalloc::profile` plus standard `snmalloc::libc::*` calls.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <snmalloc/profile/profile.h>
#include <snmalloc/profile/record.h>
#include <snmalloc/snmalloc.h>
#include <test/setup.h>
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
  // Test 1: triple-clear idempotence -- H1 then H3 then a future H4-driven
  // recursive H1 on a single populated slot.  Only the first must observe
  // the live node; the rest must return nullptr without disturbing the
  // sampled list or the node pool.
  // =========================================================================
  void test_triple_clear_idempotence()
  {
    std::cout << "test_triple_clear_idempotence\n";
    drain_global_sampled_list();

    ProfileSlot slot{nullptr};
    SampledAlloc* node = publish_sample(slot);
    check(node != nullptr, "sample published");
    if (node == nullptr)
      return;

    const size_t live_pre = SamplerGlobals::list().debug_count();
    check(live_pre >= 1, "live count >= 1 before any clear");

    // H1 (waist of Allocator::dealloc)
    SampledAlloc* first = clear_profile_slot(&slot);
    check(first == node, "first clear (H1) wins and returns the node");

    // H3 (SecondaryAllocator branch) -- on a real run this only fires
    // for pointers whose pagemap entry reports !is_owned(), but the
    // CAS contract must hold for any caller.
    SampledAlloc* second = clear_profile_slot(&slot);
    check(
      second == nullptr, "second clear (H3) is a no-op -- no double release");

    // H4 (recursive lazy-init arm of dealloc_remote_slow)
    SampledAlloc* third = clear_profile_slot(&slot);
    check(third == nullptr, "third clear (H4) is a no-op -- no double release");

    const size_t live_post = SamplerGlobals::list().debug_count();
    check(
      live_pre - live_post == 1,
      "live count decreased by exactly one across H1+H3+H4");

    drain_global_sampled_list();
  }

  // =========================================================================
  // Test 2: quadruple-clear robustness -- H1 + H2 + H3 + H4 all firing on
  // the same slot (theoretical worst case).  This guards against any
  // future refactor that introduces an extra pass through the dealloc
  // pipeline.
  // =========================================================================
  void test_quadruple_clear_robust()
  {
    std::cout << "test_quadruple_clear_robust\n";
    drain_global_sampled_list();

    ProfileSlot slot{nullptr};
    SampledAlloc* node = publish_sample(slot);
    check(node != nullptr, "sample published");
    if (node == nullptr)
      return;

    SampledAlloc* h1 = clear_profile_slot(&slot);
    SampledAlloc* h2 = clear_profile_slot(&slot);
    SampledAlloc* h3 = clear_profile_slot(&slot);
    SampledAlloc* h4 = clear_profile_slot(&slot);

    check(h1 == node, "H1 wins");
    check(h2 == nullptr, "H2 no-op");
    check(h3 == nullptr, "H3 no-op");
    check(h4 == nullptr, "H4 no-op");

    drain_global_sampled_list();
  }

  // =========================================================================
  // Test 3: nullptr robustness.  H3 is the only hook that observes
  // potentially-non-snmalloc pointers; we confirm that `record_dealloc`
  // itself early-returns on nullptr (well below the
  // find_profile_slot/clear path).  H4's path is also nullptr-safe by the
  // same logic.
  //
  // Because record_dealloc<Config> with the default Config is a
  // compile-time no-op, this is mostly a smoke test that the symbol is
  // callable with a null argument under both build flavours.
  // =========================================================================
  void test_record_dealloc_nullptr()
  {
    std::cout << "test_record_dealloc_nullptr\n";
    drain_global_sampled_list();

    // Should not crash, should not leak nodes.
    record_dealloc<snmalloc::Config>(nullptr);
    record_dealloc<snmalloc::Config>(nullptr);
    record_dealloc<snmalloc::Config>(nullptr);

    check(
      SamplerGlobals::list().debug_count() == 0,
      "nullptr record_dealloc x3 leaves list empty");
  }

  // =========================================================================
  // Test 4: cross-thread free with allocator-not-yet-initialised pressure.
  //
  // The H4 hook lives on the lazy-init arm of dealloc_remote_slow: the
  // path is taken when a thread frees a pointer it did not allocate and
  // does not yet have a local allocator.  We approximate that by
  // spawning a fresh batch of threads whose *first* action is a free of
  // a pointer allocated elsewhere.  The thread therefore enters the
  // dealloc pipeline with an uninitialised local allocator and goes
  // through `dealloc_remote_slow` -> `check_init`.
  //
  // We cannot directly assert "H4 fired" because the hook is a
  // compile-time no-op in this TU's default Config.  We assert what we
  // can: no crash, and the sampled list invariants survive.
  // =========================================================================
  void test_freshthread_remote_free()
  {
    std::cout << "test_freshthread_remote_free\n";
    drain_global_sampled_list();

    constexpr size_t N_BATCHES = 8;
    constexpr size_t PER_BATCH = 512;

    for (size_t b = 0; b < N_BATCHES; ++b)
    {
      // Allocate on the main thread, free on a brand-new thread whose
      // first action is the free.  This is the canonical scenario that
      // routes through dealloc_remote_slow's check_init lambda.
      std::vector<void*> ptrs;
      ptrs.reserve(PER_BATCH);
      for (size_t i = 0; i < PER_BATCH; ++i)
      {
        ptrs.push_back(snmalloc::libc::malloc(32 + (i & 31)));
      }

      std::thread freer([&ptrs] {
        for (auto* p : ptrs)
          snmalloc::libc::free(p);
      });
      freer.join();
    }

    check(
      SamplerGlobals::list().debug_count() == 0,
      "fresh-thread remote-free stress leaves list empty");
    check(true, "fresh-thread remote-free stress completed without crash");
  }

  // =========================================================================
  // Test 5: default-config compile-time guard.  The default Config does
  // not carry the lazy provider; both H3 and H4 must compile to a no-op
  // call.  A successful build of this TU already proves it; we add a
  // runtime confirmation that record_dealloc on a freshly-allocated
  // pointer leaves the global sampled list empty (because no slot was
  // ever populated).
  // =========================================================================
  void test_default_config_compiletime_noop()
  {
    std::cout << "test_default_config_compiletime_noop\n";

    static_assert(
      !config_has_profile_slot_v<snmalloc::Config>,
      "default Config must remain free of LazyArrayClientMetaDataProvider<"
      "ProfileSlot>");

    drain_global_sampled_list();
    void* p = snmalloc::libc::malloc(64);
    check(p != nullptr, "malloc succeeded");
    record_dealloc<snmalloc::Config>(p);
    record_dealloc<snmalloc::Config>(p);
    record_dealloc<snmalloc::Config>(p);
    snmalloc::libc::free(p);

    check(
      SamplerGlobals::list().debug_count() == 0,
      "default Config: record_dealloc x3 is a no-op");
  }
} // namespace

int main(int argc, char** argv)
{
  snmalloc::UNUSED(argc, argv);
  setup();

  std::cout << "[profile_h3_h4]\n";

#ifdef SNMALLOC_PROFILE
  std::cout << "  (SNMALLOC_PROFILE is defined: H3+H4 hooks compiled in)\n";
#else
  std::cout << "  (SNMALLOC_PROFILE is undefined: H3+H4 hooks are compile-time "
               "no-ops)\n";
#endif

  test_triple_clear_idempotence();
  test_quadruple_clear_robust();
  test_record_dealloc_nullptr();
  test_freshthread_remote_free();
  test_default_config_compiletime_noop();

  if (g_fail_count == 0)
  {
    std::cout << "[profile_h3_h4] ALL TESTS PASSED\n";
    return 0;
  }
  std::cout << "[profile_h3_h4] " << g_fail_count << " TEST(S) FAILED\n";
  return 1;
}
