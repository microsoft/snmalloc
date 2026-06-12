// SPDX-License-Identifier: MIT
//
// Implementation of the FullAllocStats getter declared in
// `src/snmalloc/global/stats_export.h` (Phase 9.1 scaffold).
//
// This compilation unit is intentionally tiny: it only needs to see the
// `Alloc::Config::Backend` accessors that already back the existing
// `malloc-extensions.cc` and `rust.cc` stats getters.  No allocator
// state is mutated; the call is a pure read.  All non-`bytes_in_use`
// / `peak_bytes_in_use` fields are zeroed via `memset` first, leaving
// the wave-2 tickets free to populate them without touching this file.

#include "../snmalloc.h"
#include "snmalloc/global/stats_export.h"

// Phase 11.6 -- lifetime histogram only needed when both PROFILE
// (the producer) and FULL (the snapshot consumer surface) are on.
#if defined(SNMALLOC_PROFILE) && defined(SNMALLOC_STATS_FULL)
#  include "snmalloc/profile/lifetime_histogram.h"
#endif

#include <string.h>

using namespace snmalloc;

extern "C" SNMALLOC_EXPORT void
snmalloc_get_full_stats(struct snmalloc_full_stats* out)
{
  if (out == nullptr)
    return;

  // Zero-fill first so every field that the wave-2 tickets haven't
  // wired up yet reads as zero -- and so the trailing `reserved[]`
  // pool and future-version slots are guaranteed to be all-zero on
  // older producers.
  memset(out, 0, sizeof(*out));

  out->version = SNMALLOC_FULL_STATS_VERSION;

  // Delegate to the existing StatsRange accounting, matching the
  // semantics of `sn_rust_statistics` and `get_malloc_info_v1`.  These
  // are static accessors on the active Config's backend; they read
  // process-global atomic counters.
  out->bytes_in_use =
    static_cast<uint64_t>(Alloc::Config::Backend::get_current_usage());
  out->peak_bytes_in_use =
    static_cast<uint64_t>(Alloc::Config::Backend::get_peak_usage());

  // Phase 9.4 -- backend fragmentation.
  //
  // `bytes_mapped` reuses the same `StatsRange` accounting that drives
  // `bytes_in_use`: snmalloc only ever has live mappings for memory it
  // also has a backend reservation for, so the two figures are
  // numerically identical at any instant.  The other two come from
  // the `BackendFragCounters` pool that `CommitRange<PAL>` writes
  // through on every `notify_using` / `notify_not_using`.
  out->bytes_mapped = out->bytes_in_use;
  {
    auto frag = snmalloc::get_backend_frag_stats();
    out->bytes_committed = frag.bytes_committed;
    out->bytes_decommitted_to_os = frag.bytes_decommitted_to_os;

    // Phase 11.4 -- copy the LargeBuddyRange free-chunk histogram
    // into the first `SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS` slots
    // of `reserved[]`.  This is the additive change that bumps the
    // wire-format version from 1 to 2.  Consumers compiled against
    // version 1 see `reserved[0..15]` as part of the opaque
    // forward-compat block and ignore it -- the change does not
    // disturb the layout of any previously-defined field above.
    static_assert(
      SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS <=
        SNMALLOC_FULL_STATS_RESERVED_SLOTS,
      "Free-chunk histogram must fit in reserved[] slot pool.");
    static_assert(
      static_cast<size_t>(SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS) ==
        snmalloc::LargeBuddyFreeChunkHistogram::NUM_BUCKETS,
      "Free-chunk histogram bucket count must match the C ABI macro.");
    for (size_t i = 0; i < SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS; ++i)
    {
      out->reserved[i] = frag.free_chunk_count_by_log_size[i];
    }
  }

  // Phase 9.5 -- lifetime histogram.
  //
  // Bump-recorded in `clear_profile_slot` (the dealloc path for
  // sampled allocations) whenever a sample completes its lifecycle.
  // Only meaningful when `SNMALLOC_PROFILE` is defined: without
  // profile support, no sample ever fires so the histogram singleton
  // is never touched and the field below stays at zero (consistent
  // with the `memset` above).  We still emit the loop under
  // `#ifdef` so a non-profile build does not link against the
  // singleton accessor.
#if defined(SNMALLOC_PROFILE) && defined(SNMALLOC_STATS_FULL)
  // Phase 11.6 -- the lifetime histogram is part of the FULL tier
  // surface.  We still require SNMALLOC_PROFILE for the bucket bumps
  // themselves to happen (profile/record.h gates the increment site),
  // but in BASIC builds we additionally skip even the snapshot read
  // here so callers observe a fully zero `lifetime_buckets_ns[]`
  // array and the BASIC build pays nothing for this surface.
  {
    auto& hist = snmalloc::profile::LifetimeHistogram::get();
    static_assert(
      snmalloc::profile::kLifetimeBuckets ==
        SNMALLOC_FULL_STATS_LIFETIME_BUCKETS,
      "LifetimeHistogram bucket count must match "
      "SNMALLOC_FULL_STATS_LIFETIME_BUCKETS");
    for (size_t i = 0; i < SNMALLOC_FULL_STATS_LIFETIME_BUCKETS; ++i)
      out->lifetime_buckets_ns[i] = hist.bucket(i);
  }
#endif

#ifdef SNMALLOC_STATS_BASIC
  // Phase 9.2 -- frontend stats aggregation (ticket 86aj0tr1e).
  // Phase 11.6 -- gated on SNMALLOC_STATS_BASIC; the per-class
  // histogram aggregation (9.3) is nested inside the FULL guard
  // below so the BASIC tier does not iterate the
  // `size_class_stats_global()` array nor read per-allocator
  // `sc_stats` blocks (the latter does not exist in the BASIC
  // build at all -- the field is `#ifdef`'d out of the
  // `Allocator` struct in `corealloc.h`).
  //
  // Sum the per-thread `FrontendStats` blocks across every live
  // allocator in the pool, then add the process-global drain
  // aggregator (populated at thread teardown by `Allocator::flush`).
  // Live allocators publish their counters non-atomically on the
  // owning thread; the cross-thread read here observes a slightly
  // stale view, which is fine for an observability snapshot.  The
  // teardown drain uses relaxed atomics so terminated-thread
  // contributions are exact.
  {
    FrontendStats agg{};
#  ifdef SNMALLOC_STATS_FULL
    SizeClassStats sc_agg{};
#  endif
    using AllocT = Allocator<Alloc::Config>;
    for (AllocT* a = AllocPool<Alloc::Config>::iterate(); a != nullptr;
         a = AllocPool<Alloc::Config>::iterate(a))
    {
      // Non-atomic read against a per-thread `stats` block.  We may
      // observe a torn 64-bit increment on 32-bit platforms, but on
      // 64-bit hosts (the ones this allocator targets) word-sized
      // loads are atomic at the hardware level.  Either way the
      // snapshot is best-effort; alignment is to the consumer.
      agg.accumulate(a->stats);
#  ifdef SNMALLOC_STATS_FULL
      sc_agg.accumulate(a->sc_stats);
#  endif
    }
    frontend_stats_global().snapshot_into(agg);
#  ifdef SNMALLOC_STATS_FULL
    size_class_stats_global().snapshot_into(sc_agg);
#  endif

    out->fast_path_allocs = agg.fast_path_allocs;
    out->slow_path_allocs = agg.slow_path_allocs;
    out->fast_path_deallocs = agg.fast_path_deallocs;
    out->remote_deallocs = agg.remote_deallocs;
    out->message_queue_drains = agg.message_queue_drains;
    out->cross_thread_messages_received =
      agg.cross_thread_messages_received;

#  ifdef SNMALLOC_STATS_FULL
    // Phase 9.3 -- copy the per-class arrays into the FFI struct.
    // `NUM_SMALL_SIZECLASSES` is statically <= the FFI slot count
    // (`SNMALLOC_FULL_STATS_SIZECLASS_SLOTS = 64`); the static
    // assert below makes that contract explicit.  Slots past
    // `NUM_SMALL_SIZECLASSES` stay zero (left clear by the
    // `memset` at the top of this function).
    //
    // Phase 11.6 -- in BASIC builds these arrays are left at zero
    // (per the `memset` above), preserving the FFI wire format so
    // existing consumers parsing `total_live_bytes_by_class` etc.
    // continue to compile and link.  Their values are simply
    // all-zero in the BASIC tier.
    static_assert(
      NUM_SMALL_SIZECLASSES <= SNMALLOC_FULL_STATS_SIZECLASS_SLOTS,
      "Per-class histogram has fewer FFI slots than snmalloc's "
      "small-class count; bump SNMALLOC_FULL_STATS_SIZECLASS_SLOTS "
      "to keep the FullAllocStats wire format wide enough.");
    for (size_t i = 0; i < NUM_SMALL_SIZECLASSES; i++)
    {
      out->total_live_bytes_by_class[i] = sc_agg.live_bytes[i];
      out->total_live_count_by_class[i] = sc_agg.live_count[i];
      // Phase 11.5 -- `cumulative_alloc` is no longer maintained
      // on the hot path; derive it here from the invariant
      //   cumulative_alloc = live_count + cumulative_dealloc.
      // The per-thread `sc_stats.cumulative_alloc[i]` field is
      // left at zero by every alloc/dealloc; this expression
      // collapses to `live + dealloc` and produces the exact same
      // value the old explicit counter would have held (a tiny
      // amount of drift is possible between a producer fast-path
      // alloc and a concurrent reader if the alloc bumped
      // `live_count` but the snapshot read both fields in the
      // opposite order -- but this is the same race the old
      // explicit field had, just shifted).
      out->cumulative_alloc_by_class[i] =
        sc_agg.live_count[i] + sc_agg.cumulative_dealloc[i];
      out->cumulative_dealloc_by_class[i] = sc_agg.cumulative_dealloc[i];
    }
#  endif // SNMALLOC_STATS_FULL
  }
#endif // SNMALLOC_STATS_BASIC
}
