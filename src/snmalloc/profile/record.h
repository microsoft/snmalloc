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

// Deliberately lightweight: this header is included from corealloc.h
// behind `#ifdef SNMALLOC_PROFILE`, and corealloc.h itself transitively
// includes everything we need (metadata.h for FrontendSlabMetadata,
// commonconfig.h for LazyArrayClientMetaDataProvider, etc).  Pulling
// commonconfig.h or metadata.h in here directly would create a cycle:
//   commonconfig.h -> mem/mem.h -> mem/corealloc.h -> profile/record.h.
//
// Consumers that include profile/record.h *without* having corealloc.h
// already in scope (none today) must arrange for those headers to be
// available at template-instantiation time.

#include "../ds_core/defines.h"
#include "node_pool.h"
#include "reentrancy_guard.h"
#include "sampled_alloc.h"
#include "sampled_list.h"
#include "sampler.h"

#include <atomic>
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

      // Step 1: re-entrancy.  If the sampler is already live on this
      // thread, do nothing.  This can happen when the profile subsystem
      // itself triggers a dealloc during cleanup; we must not recurse.
      if (sampler_reentered())
        return;

      ReentrancyGuard guard;

      // Step 2: find the slot.  Returns nullptr if the lazy backing is
      // not yet installed for this slab -- common case until something
      // on this slab has been sampled.
      ProfileSlot* slot = find_profile_slot<Config>(p);
      if (slot == nullptr)
        return;

      // Step 3: atomic clear + cleanup.
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

      // Sampler::record_alloc has its own internal re-entrancy short-
      // circuit, so we do not need an outer guard here.  The slow path
      // inside the sampler builds a ReentrancyGuard before doing any
      // payload work (NodePool acquire, stack walk, list push).
      const uintptr_t addr = reinterpret_cast<uintptr_t>(p);
      const bool fired = tl_sampler.record_alloc(addr, requested, allocated);
      if (SNMALLOC_LIKELY(!fired))
        return;

      SampledAlloc* node = tl_sampler.last_sample();
      if (node == nullptr)
      {
        // Sample fired logically but pool exhausted (or sampler
        // re-entered).  Nothing to install.
        return;
      }

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
      }
    }
  }
} // namespace snmalloc::profile
