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

#ifdef SNMALLOC_PROFILE
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
#ifdef SNMALLOC_PROFILE
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

#ifdef SNMALLOC_STATS
  // Phase 9.2 -- frontend stats aggregation (ticket 86aj0tr1e).
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
    }
    frontend_stats_global().snapshot_into(agg);

    out->fast_path_allocs = agg.fast_path_allocs;
    out->slow_path_allocs = agg.slow_path_allocs;
    out->fast_path_deallocs = agg.fast_path_deallocs;
    out->remote_deallocs = agg.remote_deallocs;
    out->message_queue_drains = agg.message_queue_drains;
    out->cross_thread_messages_received =
      agg.cross_thread_messages_received;
  }
#endif
}
