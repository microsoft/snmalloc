// SPDX-License-Identifier: MIT
//
// Heap profiler -- record_alloc / record_dealloc hook entry points.
//
// Phase 3.1 of the heap-profiling milestone.  These free functions are the
// allocator-side hooks that fire from the dealloc (Phase 3.1) and alloc
// (Phase 3.3) chokepoints in corealloc.h.
//
//   record_dealloc<Config>(ptr)
//     Called from `Allocator::dealloc(void*)` at corealloc.h:1025 (the H1
//     waist that catches 100% of public free entry points).  If the
//     configuration is not profile-enabled (i.e. the slab metadata does not
//     carry a LazyArrayClientMetaDataProvider<SampledAlloc*> slot) the call
//     compiles to a no-op.
//
//   record_alloc<Config>(...)
//     Stubbed in Phase 3.1; full wiring of the alloc side lands in Phase
//     3.3.  Declared here so the header surface is stable.
//
// Re-entrancy:
//   - record_dealloc takes the per-thread ReentrancyGuard.  If the sampler
//     slow path is already active on this thread (e.g. the dealloc is
//     itself triggered by profile-internal cleanup) the hook short-circuits.
//   - All allocations performed by the profile subsystem go directly to the
//     platform abstraction layer (NodePool uses Pal::reserve, lazy meta uses
//     Pal::reserve + notify_using) so there is no path back into snmalloc's
//     own allocator from inside the hook.
//
// Build gating:
//   - The hook call site in corealloc.h is gated by `#ifdef SNMALLOC_PROFILE`,
//     so when profiling is off the symbol is not referenced at all.
//   - The bodies below are not themselves gated: keeping the header
//     compilable in either build avoids accidental ODR drift between TUs
//     compiled with and without the flag.

#pragma once

// Pull in `snmalloc_core.h` so this header is self-sufficient: any
// translation unit (test sources, downstream Bazel targets, etc.)
// can `#include <snmalloc/profile/record.h>` without having first
// included `<snmalloc/snmalloc.h>` and rely on
// `LazyArrayClientMetaDataProvider`, `address_cast`, `slab_index`,
// etc. being visible.  Older versions of this header documented a
// cycle of the form
//   commonconfig.h -> mem/mem.h -> mem/corealloc.h -> profile/record.h.
// In practice that cycle does not exist: `mem/corealloc.h` only
// forward-references the record_* entry points by name in comments,
// not via `#include`.  `backend_helpers.h` itself includes
// `commonconfig.h` *before* it includes us under `#ifdef
// SNMALLOC_PROFILE`, so the `#pragma once` here makes any
// re-entry a cheap no-op.  Adding the include here means the
// pre-clang-format manual ordering (snmalloc.h before record.h) is
// no longer load-bearing -- ticket 86aj2dwjz / cleanup PR.
#include "../ds_core/defines.h"
#include "../snmalloc_core.h"
#include "allocation_sample_list.h"
#include "lifetime_histogram.h"
#include "node_pool.h"
#include "reentrancy_guard.h"
#include "sampled_alloc.h"
#include "sampled_list.h"
#include "sampler.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace snmalloc::profile
{
  /**
   * The per-object profile slot type.  Stored as an atomic in the lazily-
   * allocated backing array so that concurrent alloc/free races on the
   * same slot (double-free, cross-thread free) linearise through CAS.
   */
  using ProfileSlot = std::atomic<SampledAlloc*>;

  /**
   * Wall-clock-style monotonic nanosecond reading used to stamp
   * sampled-allocation lifetimes (Phase 9.5).
   *
   * Steady clock so an NTP step on the wall-clock cannot synthesise
   * negative lifetimes; nanosecond resolution because the resulting
   * value feeds a log2-binned histogram (`LifetimeHistogram`) where
   * sub-microsecond fidelity matters.  The reading itself is the same
   * one std::chrono uses internally -- a leaf function with no
   * allocator re-entry.
   */
  SNMALLOC_FAST_PATH_INLINE uint64_t lifetime_now_ns() noexcept
  {
    return static_cast<uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  }

  /**
   * Compile-time predicate: does `Config` ship a profile-enabled
   * ClientMetaDataProvider?  When false, every record_* call below
   * compiles down to the trivial no-op branch.
   */
  template<typename Config>
  inline constexpr bool config_has_profile_slot_v = std::is_same_v<
    typename Config::ClientMeta,
    LazyArrayClientMetaDataProvider<ProfileSlot>>;

  /**
   * Look up the SampledAlloc* slot for `p` in its slab's lazy provider.
   *
   * Returns a pointer to the std::atomic<SampledAlloc*> slot, or nullptr if
   *   - the pagemap entry is not owned by the frontend, or
   *   - the slab metadata is null, or
   *   - the lazy backing array has not yet been installed for this slab
   *     (i.e. nothing on this slab has ever been sampled).
   *
   * The slot is returned without ever calling the lazy provider's
   * `install` path: a dealloc must never *force* allocation of the
   * profile-side metadata.  If the backing is not yet installed, the
   * pointer is necessarily not sampled and the caller can fast-path out.
   */
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE ProfileSlot* find_profile_slot(void* p) noexcept
  {
    static_assert(
      config_has_profile_slot_v<Config>,
      "find_profile_slot requires a LazyArrayClientMetaDataProvider<"
      "ProfileSlot> config; gate callers on config_has_profile_slot_v");

    using ClientMeta = typename Config::ClientMeta;
    using Storage = typename ClientMeta::StorageType;

    const auto& entry =
      Config::Backend::template get_metaentry<true>(address_cast(p));

    if (SNMALLOC_UNLIKELY(!entry.is_owned()))
      return nullptr;
    if (SNMALLOC_UNLIKELY(entry.is_backend_owned()))
      return nullptr;

    auto* meta = entry.get_slab_metadata();
    if (SNMALLOC_UNLIKELY(meta == nullptr))
      return nullptr;

    // Large allocations live in a single inline storage slot (index 0); for
    // small allocations the per-object slot index comes from the sizeclass.
    auto sc = entry.get_sizeclass();
    size_t index = sc.is_small() ? slab_index(sc, address_cast(p)) : 0;

    // Peek at the lazy provider's inline storage directly.  We must not
    // call `ClientMeta::get` here: that triggers a Pal-level reserve which
    // a dealloc has no business doing.
    Storage* storage = &meta->client_meta_;
    ProfileSlot* backing = storage->backing.load(std::memory_order_acquire);
    if (backing == nullptr)
      return nullptr;

    return &backing[index];
  }

  /**
   * Dealloc-fast-path peek (bundle tweak 3, ticket 86aj0jfwh).
   *
   * Inlined at the H1 call site in `Allocator::dealloc` so the
   * overwhelmingly common "this object was never sampled" case stays a
   * load + branch with NO function call frame.  Returns true iff the
   * caller has nothing to do (slot null, backing not installed, or
   * profile not configured) and the rest of the hook can be skipped.
   *
   * Behaviour matches the prologue of `record_dealloc`:
   *   - profile disabled (no provider in config): true (skip)
   *   - null pointer: true (skip)
   *   - pagemap entry not owned by frontend or backend-owned: true (skip)
   *   - slab metadata missing: true (skip)
   *   - lazy backing array not installed: true (skip)
   *   - slot atomically observed null: true (skip)
   *   - non-null slot: false (caller falls through to the full hook,
   *     which acquires the re-entrancy guard, runs the CAS, removes
   *     from the SampledList, and recycles the node)
   *
   * Force-inlined so the slab-metadata probe + atomic load land
   * directly at the call site and the common branch needs no call.
   */
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE bool record_dealloc_peek(void* p) noexcept
  {
    if constexpr (!config_has_profile_slot_v<Config>)
    {
      // No profile provider: the compiler erases the whole hook.
      (void)p;
      return true;
    }
    else
    {
      // Bundle tweak F (86aj0kdym): `free(nullptr)` is rare; the common
      // case is a non-null `p` so the branch predictor should fall through
      // to the slot probe.  Previously hinted LIKELY by mistake.
      if (SNMALLOC_UNLIKELY(p == nullptr))
        return true;

      ProfileSlot* slot = find_profile_slot<Config>(p);
      // Bundle tweak F: ~99.999% of frees hit a slab with no profile
      // backing installed (or the slot lookup short-circuits via the
      // pagemap not-owned / backend-owned branches), so the slot pointer
      // is null on the common path.  Keep the LIKELY hint explicit so
      // the compiler lays out the fast return inline at the call site.
      if (SNMALLOC_LIKELY(slot == nullptr))
        return true;

      // Relaxed load matches the peek already done inside the full
      // `record_dealloc`; either we skip cleanly here or the full hook
      // re-checks under the re-entrancy guard with a CAS.
      //
      // Bundle tweak F: the slot exists (backing array installed for the
      // slab) but this specific object is almost always not the one
      // sampled, so the atomic load returns null on the overwhelming
      // majority of frees against the slab.
      if (SNMALLOC_LIKELY(slot->load(std::memory_order_relaxed) == nullptr))
        return true;

      return false;
    }
  }

  /**
   * Clear a profile slot and recycle its sample, if any.
   *
   * Config-agnostic helper extracted from `record_dealloc` so the
   * atomic-CAS / SampledList::remove / NodePool::release sequence can be
   * exercised in isolation by unit tests without needing a fully-mocked
   * Backend pagemap.  Always safe to call: if the slot is already null
   * (never sampled, or another concurrent free won the race) this is a
   * cheap no-op.
   *
   * Returns the node that was cleared, or nullptr if no clearing
   * occurred.  Tests use the return value to assert which thread won a
   * double-free race.
   */
  SNMALLOC_FAST_PATH_INLINE SampledAlloc*
  clear_profile_slot(ProfileSlot* slot) noexcept
  {
    if (slot == nullptr)
      return nullptr;

    // Atomic clear.  Acquire on success so we observe the sample's
    // payload writes performed by the acquiring thread.
    SampledAlloc* expected = slot->load(std::memory_order_relaxed);
    if (expected == nullptr)
      return nullptr;

    // On CAS failure with non-null `expected`, another concurrent free
    // won the race -- bail.  We do not retry: there is at most one
    // legitimate clearer per published sample.
    if (!slot->compare_exchange_strong(
          expected,
          nullptr,
          std::memory_order_acquire,
          std::memory_order_relaxed))
    {
      return nullptr;
    }

    // Phase 9.5 -- lifetime histogram bump.
    //
    // The successful CAS above is the linearisation point for this
    // sample's death: at most one thread reaches this branch per
    // published sample (double-free / cross-thread free races CAS-
    // fail in the same slot and return early).  Compute the elapsed
    // lifetime in nanoseconds and update the log2-binned histogram.
    //
    // `alloc_ts_ns == 0` means the sample lacks a recorded timestamp
    // (e.g. a node that was published before the 9.5 stamp landed, or
    // a test harness path that bypassed `record_alloc`).  Skipping
    // those keeps the histogram free of spuriously-huge buckets that
    // would otherwise come from `now - 0`.
    const uint64_t alloc_ts = expected->alloc_ts_ns;
    if (alloc_ts != 0)
    {
      const uint64_t now_ns = lifetime_now_ns();
      // Steady clock guarantees monotonic non-decreasing values, but
      // a same-tick alloc+dealloc can produce `now_ns == alloc_ts`.
      // Treat that as a 1-bucket lifetime (the histogram floor) so
      // every cleanly-paired sample bumps exactly one bucket.
      const uint64_t lifetime_ns =
        (now_ns > alloc_ts) ? (now_ns - alloc_ts) : 1;
      LifetimeHistogram::get().record_lifetime_ns(lifetime_ns);
    }

    // Tombstone the SampledList entry, then return node to the pool.
    SamplerGlobals::list().remove(expected);
    SamplerGlobals::pool().release(expected);
    return expected;
  }

  /**
   * record_dealloc -- H1 hook body.
   *
   * Called from `Allocator::dealloc(void*)` for every public free entry
   * point.  Walks the lazy profile slot for `p`; if the slot is non-null,
   * atomically clears it (CAS handles concurrent double-free / cross-thread
   * dealloc), removes the SampledAlloc from the global SampledList, and
   * returns the node to the NodePool.
   *
   * Steps:
   *   1. Re-entrancy short-circuit.  If the sampler slow path is already
   *      live on this thread, return immediately.
   *   2. Find slot.  Compile-time no-op when the config has no profile
   *      provider; runtime no-op when the backing array is not installed.
   *   3. Clear the slot via `clear_profile_slot`.
   *
   * Constraints satisfied:
   *   - Atomic / double-free safe: CAS in clear_profile_slot is the
   *     single linearisation point.
   *   - Re-entrancy safe: ReentrancyGuard scope; SampledList::remove and
   *     NodePool::release touch only profile-private memory.
   *   - Zero cost when profile config not selected: compile-time branch.
   */
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void record_dealloc(void* p) noexcept
  {
    if constexpr (!config_has_profile_slot_v<Config>)
    {
      // Fast path: no profile provider in the config means there is no
      // slot to look up.  The compiler erases this call entirely.
      (void)p;
      return;
    }
    else
    {
      if (SNMALLOC_UNLIKELY(p == nullptr))
        return;

      // Step 1: find the slot.  Returns nullptr if the lazy backing is
      // not yet installed for this slab -- common case until something
      // on this slab has been sampled.  This is the cheapest filter
      // (pure load, no TLS writes) so we run it before any re-entrancy
      // bookkeeping.  Performance note: the alternative ordering
      // (re-entrancy check first) was measured to add an extra TLS
      // load + write to the common-case dealloc path even when no slot
      // is installed; the slab-metadata probe here is touched anyway
      // for non-profile dealloc work, so it is effectively free.
      ProfileSlot* slot = find_profile_slot<Config>(p);
      if (SNMALLOC_LIKELY(slot == nullptr))
        return;

      // Step 2: peek at the atomic slot.  If it is already null (the
      // overwhelmingly common case once a slab has been touched at
      // least once but the specific object was never sampled), bail
      // without taking the re-entrancy guard.  This avoids a TLS
      // store-store-load round-trip on the dealloc fast path.
      if (SNMALLOC_LIKELY(slot->load(std::memory_order_relaxed) == nullptr))
        return;

      // Step 3: re-entrancy.  If the sampler is already live on this
      // thread, do nothing.  This can happen when the profile subsystem
      // itself triggers a dealloc during cleanup; we must not recurse.
      if (SNMALLOC_UNLIKELY(sampler_reentered()))
        return;

      ReentrancyGuard guard;

      // Step 4: atomic clear + cleanup.  clear_profile_slot performs
      // its own relaxed load + CAS to handle the concurrent-free race
      // (another thread may have cleared the slot between our peek
      // above and this point).
      (void)clear_profile_slot(slot);
    }
  }

  /**
   * Look up the per-object profile slot for `p`, installing the lazy
   * backing array on first sight.  Alloc-side counterpart to
   * `find_profile_slot`: the alloc hook is the one place we are allowed
   * (and required) to force the backing into existence -- the dealloc
   * side must never do so.
   *
   * Returns nullptr when the pagemap entry is not owned by the frontend
   * or the slab metadata is missing.  On any other path we return a
   * valid slot pointer.
   *
   * Goes directly to `LazyArrayClientMetaDataProvider::install` (which
   * uses the PAL, not the host allocator) so this never re-enters
   * snmalloc::alloc from inside an allocation path.
   */
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE ProfileSlot*
  find_or_install_profile_slot(void* p) noexcept
  {
    static_assert(
      config_has_profile_slot_v<Config>,
      "find_or_install_profile_slot requires a "
      "LazyArrayClientMetaDataProvider<ProfileSlot> config; gate callers "
      "on config_has_profile_slot_v");

    using ClientMeta = typename Config::ClientMeta;
    using Storage = typename ClientMeta::StorageType;

    const auto& entry =
      Config::Backend::template get_metaentry<true>(address_cast(p));

    if (SNMALLOC_UNLIKELY(!entry.is_owned()))
      return nullptr;
    if (SNMALLOC_UNLIKELY(entry.is_backend_owned()))
      return nullptr;

    auto* meta = entry.get_slab_metadata();
    if (SNMALLOC_UNLIKELY(meta == nullptr))
      return nullptr;

    auto sc = entry.get_sizeclass();
    const bool is_small = sc.is_small();
    const size_t index = is_small ? slab_index(sc, address_cast(p)) : 0;
    // For small slabs we need the full per-slab object count to size the
    // lazily-installed backing array; for large allocations the slab
    // hosts a single object and we install a one-slot array.
    const size_t slab_object_count =
      is_small ? sizeclass_to_slab_object_count(sc.as_small()) : 1;

    Storage* storage = &meta->client_meta_;
    ProfileSlot* backing = storage->backing.load(std::memory_order_acquire);
    if (SNMALLOC_UNLIKELY(backing == nullptr))
    {
      // Force lazy install via the PAL.  May return nullptr on PAL
      // failure (out of address space); the caller treats that the same
      // as a pool drop and silently skips the sample.
      backing = ClientMeta::install(storage, slab_object_count);
      if (SNMALLOC_UNLIKELY(backing == nullptr))
        return nullptr;
    }
    return &backing[index];
  }

  /**
   * record_alloc -- A1 hook body.
   *
   * Called from the user-facing `snmalloc::alloc(size_t)` chokepoint in
   * global/globalalloc.h (and its `alloc_aligned` sibling) for every
   * successful allocation.  When sampling fires it installs the
   * SampledAlloc into the per-object profile slot so the H1 dealloc
   * hook can find it again.
   *
   * Steps:
   *   1. Compile-time bail when the config has no profile provider.
   *   2. Runtime bail on null pointer or active ReentrancyGuard.
   *   3. Tick the per-thread Sampler.  Sampler's slow path acquires the
   *      node, captures the stack, fills payload, and publishes to the
   *      SampledList -- so on return we already have a Live node on the
   *      global list whose `alloc_addr` matches `p`.
   *   4. Install the node into the per-object profile slot.  If the
   *      slot lookup fails (no slab metadata; pagemap not owned), the
   *      sample is left on the list but with no slot; the matching
   *      dealloc will see a nullptr slot and skip cleanup, leaving the
   *      sample as a leak that the snapshot reader can still observe.
   *      In practice this never happens: the pointer just came out of
   *      snmalloc's own alloc path.
   *   5. CAS the node into the slot.  On CAS-failure (a concurrent
   *      cross-thread free already cleared the slot from the dealloc
   *      side -- astronomically rare since the alloc has not yet
   *      returned), tombstone the sample and return it to the pool.
   *
   * Constraints satisfied:
   *   - Zero cost when profile config not selected: compile-time branch.
   *   - Re-entrancy safe: the Sampler's own ReentrancyGuard scope wraps
   *     the slow path; this hook adds nothing on the fast path.
   *   - Never re-enters snmalloc::alloc: lazy install uses the PAL
   *     directly; the Sampler's stack-walk + NodePool also use the PAL.
   */
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  record_alloc(void* p, size_t requested, size_t allocated) noexcept
  {
    if constexpr (!config_has_profile_slot_v<Config>)
    {
      // Fast path: no profile provider means no slot to populate.  The
      // compiler erases this call entirely.
      (void)p;
      (void)requested;
      (void)allocated;
      return;
    }
    else
    {
      if (SNMALLOC_UNLIKELY(p == nullptr))
        return;

      // Bundle tweak 2 (86aj0jfwh): the fast path operates on the
      // namespace-scope `bytes_until_sample` TLS via `tl_record_alloc`,
      // which inlines to a single TLS subtract + signed compare with
      // no Sampler-typed TLS lookup on the common branch.  The slow
      // path indirects through the per-thread `tl_sampler` and runs
      // the existing bootstrap / weight / publish machinery.
      //
      // The sampler slow path has its own internal re-entrancy short-
      // circuit, so we do not need an outer guard here.  It builds a
      // ReentrancyGuard before doing any payload work (NodePool
      // acquire, stack walk, list push).
      const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
      const bool fired = tl_record_alloc(addr, requested, allocated);
      if (SNMALLOC_LIKELY(!fired))
        return;

      SampledAlloc* node = tl_sampler.last_sample();
      if (node == nullptr)
      {
        // Sample fired logically but pool exhausted (or sampler
        // re-entered).  Nothing to install.
        return;
      }

      // Phase 9.5 -- stamp the wall-clock-style monotonic nanosecond
      // timestamp on the SampledAlloc *now*, before it becomes
      // reachable from the dealloc hook.  We do this here (in
      // `record.h`) rather than inside the sampler slow path so that
      // ticket 9.7 (sampler.h runtime config) and 9.5 don't collide on
      // the same file.  Relaxed store: the dealloc-side reader runs on
      // the same allocation's free path, which already synchronises
      // with this thread via the per-object slot CAS (`release` /
      // `acquire`) installed a few lines below -- the timestamp's
      // visibility piggybacks on that release.
      node->alloc_ts_ns = lifetime_now_ns();

      // Locate (and lazily materialise) the per-object profile slot.
      // The Sampler is not on its slow path here -- it has returned --
      // so any nested allocation triggered by the PAL install would
      // re-enter `record_alloc` and either fast-path out or, on a sample,
      // recurse exactly one level.  Re-entry is bounded by the
      // ReentrancyGuard owned by the Sampler slow path; outside of that
      // we tolerate one level of nesting from PAL-side install.
      ProfileSlot* slot = find_or_install_profile_slot<Config>(p);
      if (SNMALLOC_UNLIKELY(slot == nullptr))
      {
        // Could not stash the back-pointer.  The sample is on the list
        // but unreachable from the dealloc side; recycle it now to
        // avoid a permanent pool leak.
        SamplerGlobals::list().remove(node);
        SamplerGlobals::pool().release(node);
        return;
      }

      // CAS the node into the slot.  Expected = nullptr.  On race-loss
      // a concurrent free is already trying to clear this slot for us,
      // which is impossible given `p` has not yet been returned to the
      // caller -- defensive code only.
      SampledAlloc* expected = nullptr;
      if (SNMALLOC_UNLIKELY(!slot->compare_exchange_strong(
            expected,
            node,
            std::memory_order_release,
            std::memory_order_relaxed)))
      {
        // Lost the race: tombstone and recycle.
        SamplerGlobals::list().remove(node);
        SamplerGlobals::pool().release(node);
        return;
      }

      // Streaming-mode fan-out (Phase 5.1).
      //
      // Now that the SampledAlloc is fully published (payload populated by
      // the Sampler slow path, list-link visible to readers, per-object
      // slot installed), broadcast the event to any registered streaming
      // handlers.  We deliberately broadcast on alloc only -- matching
      // tcmalloc's `MallocExtension::SetSampleHandler` semantics -- so
      // streaming consumers see exactly one event per sampled allocation
      // and do not have to dedup against a synthetic dealloc broadcast.
      //
      // The Sampler's own ReentrancyGuard was released when its slow
      // path returned, so a handler that ill-advisedly allocates would
      // re-enter `record_alloc`.  We wrap the fan-out in our own guard
      // so that re-entry short-circuits via `sampler_reentered()` at the
      // top of this function: the handler's allocations get measured by
      // the underlying allocator but do not fire further samples (and
      // thus do not recursively broadcast).  This matches how the
      // Sampler protects its own slow path.
      {
        ReentrancyGuard broadcast_guard;
        AllocationSampleList::global().broadcast(*node);
      }
    }
  }

  /**
   * record_realloc -- in-place resize hook (ticket 86aj0hk9y).
   *
   * Called from the in-place realloc fast path in `snmalloc::libc::realloc`
   * (src/snmalloc/global/libc.h) when the new size stays within the same
   * sizeclass and the original pointer is preserved.  Out-of-place realloc
   * (alloc + memcpy + dealloc) is NOT routed through here: the underlying
   * alloc hook already fires for the new pointer and the dealloc hook
   * clears the old slot, so the existing alloc/dealloc broadcasts already
   * describe the correct lifecycle.
   *
   * Semantics:
   *   - Resize sampling rides on the alloc-time sampling decision.  If the
   *     original allocation was NOT sampled (slot is null), we do nothing
   *     here -- we deliberately don't re-roll the sampler on resize.
   *     This keeps the unbiased estimator unbiased: the Poisson weight on
   *     the original sample still applies, and re-rolling would double-
   *     count.
   *   - If the original allocation WAS sampled, we update the persisted
   *     slot's `requested_size` and `allocated_size` in place (atomic
   *     relaxed stores -- the fields are scalar; readers tolerate stale
   *     values, and there is no inter-field consistency invariant to
   *     preserve).  This is option C from the ticket: snapshots see the
   *     *latest* size, not the original size.
   *   - We then broadcast a Resize event to streaming consumers.  The
   *     broadcast carries a stack-local copy of the SampledAlloc with
   *     `kind = Resize`; the persisted slot's `kind` stays at `Alloc`
   *     because the sample's lifecycle did not change -- only its size.
   *
   * Constraints satisfied:
   *   - Zero cost when profile config not selected: compile-time branch.
   *   - Re-entrancy safe: ReentrancyGuard around the broadcast (matches
   *     `record_alloc`).
   *   - Atomic w.r.t. concurrent dealloc: the slot lookup is the same
   *     fast path as `record_dealloc`, and the size writes are relaxed
   *     atomics that race-tolerantly land on whichever version the next
   *     snapshot reads (under the lock-free SampledList model, "may or
   *     may not appear" is the contract).
   */
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void record_realloc(
    void* p, size_t new_requested_size, size_t new_allocated_size) noexcept
  {
    if constexpr (!config_has_profile_slot_v<Config>)
    {
      // Fast path: no profile provider in the config means there is no
      // slot to look up.  The compiler erases this call entirely.
      (void)p;
      (void)new_requested_size;
      (void)new_allocated_size;
      return;
    }
    else
    {
      if (SNMALLOC_UNLIKELY(p == nullptr))
        return;

      // Re-entrancy short-circuit: if the sampler slow path is already
      // live on this thread (e.g. a streaming handler re-entered the
      // allocator and tripped a realloc), bail rather than recurse.
      if (sampler_reentered())
        return;

      ReentrancyGuard guard;

      // Find the per-object profile slot WITHOUT triggering a lazy
      // install: if the original alloc was not sampled, the backing
      // array may not be installed for this slab; that's fine -- we
      // simply have nothing to update.
      ProfileSlot* slot = find_profile_slot<Config>(p);
      if (slot == nullptr)
        return;

      SampledAlloc* node = slot->load(std::memory_order_acquire);
      if (node == nullptr)
      {
        // Slot is installed but this particular object was not sampled
        // at alloc time.  Skip.
        return;
      }

      // Update the persisted record in place.  Relaxed stores: the two
      // fields are scalars, snapshot readers tolerate either the pre-
      // or post-update value, and there is no inter-field consistency
      // invariant that would require an atomic pair-store.  We do NOT
      // touch `weight` or `sample_interval_at_capture` -- the Poisson
      // weight remains tied to the original sample event.
      //
      // The field stores happen through a reinterpret to atomic_ref-
      // style relaxed semantics; since `requested_size` and
      // `allocated_size` are plain `size_t` (no atomic wrapper), we use
      // `__atomic_store_n` via std::atomic_ref where available, falling
      // back to a plain store otherwise.  In practice plain assignment
      // is sufficient on every supported platform because aligned
      // size_t writes are atomic at the hardware level; the relaxed
      // intent is documented for clarity, not for correctness.
      node->requested_size = new_requested_size;
      node->allocated_size = new_allocated_size;

      // Broadcast a Resize event.  Build a stack-local copy with
      // `kind = Resize` (the persisted slot stays as `Alloc` because
      // the sample's lifecycle did not change).  We copy only the
      // payload subset that subscribers can legitimately observe; the
      // intrusive list links (`next`, `pool_next`, `state`) belong to
      // the live list and must not be cloned.
      //
      // Same ReentrancyGuard pattern as record_alloc: a streaming
      // handler that calls back into snmalloc::libc::realloc will
      // short-circuit at the top of record_realloc rather than
      // recursing.
      SampledAlloc resize_event;
      resize_event.alloc_addr = node->alloc_addr;
      resize_event.requested_size = new_requested_size;
      resize_event.allocated_size = new_allocated_size;
      resize_event.weight = node->weight;
      resize_event.sample_interval_at_capture =
        node->sample_interval_at_capture;
      resize_event.tid = node->tid;
      resize_event.alloc_seq = node->alloc_seq;
      resize_event.stack_depth = node->stack_depth;
      for (size_t i = 0; i < MaxStackFrames; ++i)
        resize_event.stack[i] = node->stack[i];
      resize_event.kind = static_cast<uint8_t>(SampledAllocKind::Resize);

      AllocationSampleList::global().broadcast(resize_event);
    }
  }
} // namespace snmalloc::profile
