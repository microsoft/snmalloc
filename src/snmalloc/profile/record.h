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
   * record_alloc -- A1 hook body.  Phase 3.3 wires this up; in Phase 3.1
   * it is a stub so the header surface is stable.  The intended signature
   * is `record_alloc<Config>(ptr, requested_size, allocated_size)`; we
   * leave the parameter list narrow for now since no caller exists.
   *
   * When invoked it must:
   *   1. Tick the per-thread Sampler with `requested_size`.
   *   2. On a sample fire: acquire a NodePool node, capture the stack,
   *      populate the SampledAlloc, publish it on the SampledList, and
   *      install it in the per-object profile slot for `p` (forcing the
   *      lazy backing to materialise via ClientMeta::get).
   *
   * Until Phase 3.3 this is intentionally a no-op so corealloc.h's H1
   * hook compiles in isolation.
   */
  template<typename Config>
  SNMALLOC_FAST_PATH_INLINE void
  record_alloc(void* p, size_t requested, size_t allocated) noexcept
  {
    (void)p;
    (void)requested;
    (void)allocated;
  }
} // namespace snmalloc::profile
