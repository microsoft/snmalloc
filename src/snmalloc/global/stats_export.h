// SPDX-License-Identifier: MIT
//
// FullAllocStats scaffold (Phase 9.1).
//
// Public C ABI surface for the broader Phase 9 telemetry work.  Carries
// the layout of `struct snmalloc_full_stats` and the prototype of the
// `snmalloc_get_full_stats` getter that lives in
// `src/snmalloc/override/stats_export.cc`.
//
// This header intentionally exposes ONLY POD types and uses fixed-width
// integers from `<stdint.h>` so the layout is stable across:
//
//   * the C ABI consumed by the Rust binding in `snmalloc-sys`;
//   * any other in-tree C++ consumer that wants to read aggregated
//     telemetry without depending on the (much larger) C++ Config
//     template surface.
//
// The struct is the shared write target for the wave-2 Phase 9
// tickets:
//
//   * 9.2 — fast/slow path alloc/dealloc and cross-thread message
//           counters
//   * 9.3 — per-size-class live / cumulative byte and count histograms
//   * 9.4 — `bytes_mapped` / `bytes_committed` /
//           `bytes_decommitted_to_os`
//   * 9.5 — `lifetime_buckets_ns` allocation-lifetime histogram
//
// At this scaffold stage every field except `bytes_in_use` and
// `peak_bytes_in_use` is zeroed.  The two live fields delegate to
// `snmalloc::StatsRange::get_current_usage` /
// `snmalloc::StatsRange::get_peak_usage`, i.e. the same source that
// already backs the Rust `SnMalloc::memory_stats()` getter.

#pragma once

#include <stdint.h>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

/**
 * Wire-format version for `struct snmalloc_full_stats`.
 *
 * Incremented when the struct gains a new field at a previously-reserved
 * slot (Phase 9 wave-2 tickets) or when the trailing `reserved[]` block
 * is consumed.  Consumers should read this field first and treat any
 * value greater than the version they were compiled against as
 * "additional fields present, ignored" -- the prefix layout is stable.
 *
 * History:
 *
 *   1 -- initial wire format (Phase 9.1 scaffold + waves 9.2-9.6).
 *
 *   2 -- Phase 11.4: `reserved[0..15]` is now the
 *        `LargeBuddyRange` free-chunk histogram (log2-bucketed counts
 *        of currently-free chunks at sizes
 *        `1 << (MIN_CHUNK_BITS + i)` for `i` in
 *        `[0, SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS - 1]`).  Older
 *        version-1 consumers that ignore the reserved block continue
 *        to read the same `bytes_committed` /
 *        `bytes_decommitted_to_os` values: the change is strictly
 *        additive within the existing reserved slot pool, so the
 *        offsets of every previously-defined field are preserved.
 */
#define SNMALLOC_FULL_STATS_VERSION 2u

/**
 * Number of log2 buckets occupied by the Phase 11.4 free-chunk
 * histogram.  The histogram lives in `reserved[0..N-1]` where
 * `N == SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS`; bucket `i` carries
 * the count of currently-free chunks of size
 * `1 << (MIN_CHUNK_BITS + i)` bytes held inside any
 * `LargeBuddyRange` Buddy.
 */
#define SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS 16u

/**
 * Number of size-class slots reserved in the per-class histograms.
 * snmalloc has 64 small-object size classes plus 18 large-object
 * classes; the scaffold reserves the widest slot (64) so the 9.3
 * implementation can populate without renegotiating the layout.
 */
#define SNMALLOC_FULL_STATS_SIZECLASS_SLOTS 64u

/**
 * Number of histogram buckets for the allocation-lifetime distribution
 * (Phase 9.5).  Sized to cover a wide log2-spaced range from
 * nanoseconds to days without forcing a layout change later.
 */
#define SNMALLOC_FULL_STATS_LIFETIME_BUCKETS 32u

/**
 * Trailing reserved slots for forward-compatible additions.  New fields
 * in subsequent revisions are taken from this pool; the
 * `SNMALLOC_FULL_STATS_VERSION` macro tells consumers which fields are
 * actually live.
 */
#define SNMALLOC_FULL_STATS_RESERVED_SLOTS 64u

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * Aggregated allocator telemetry snapshot.  Bit-for-bit identical
   * across the C / Rust FFI boundary.
   *
   * Field semantics:
   *
   *   `version`
   *     Wire-format version (`SNMALLOC_FULL_STATS_VERSION` at the time
   *     the producer was built).  Always populated.
   *
   *   `bytes_in_use` / `peak_bytes_in_use`
   *     OS-level reservation bytes, range granularity (not the count of
   *     live individual allocations).  Sourced from the existing
   *     `StatsRange` accounting; identical numbers to what the Rust
   *     `SnMalloc::memory_stats()` getter returns.
   *
   *   `bytes_mapped` / `bytes_committed` / `bytes_decommitted_to_os`
   *     Reserved for Phase 9.4; zero at the scaffold stage.
   *
   *   `fast_path_allocs` / `slow_path_allocs` / `fast_path_deallocs` /
   *   `remote_deallocs` / `message_queue_drains` /
   *   `cross_thread_messages_received`
   *     Reserved for Phase 9.2; zero at the scaffold stage.
   *
   *   `total_live_bytes_by_class[]` / `total_live_count_by_class[]` /
   *   `cumulative_alloc_by_class[]` / `cumulative_dealloc_by_class[]`
   *     Reserved for Phase 9.3; zero at the scaffold stage.  Indexed by
   *     snmalloc small-object size class.
   *
   *   `lifetime_buckets_ns[]`
   *     Reserved for Phase 9.5; zero at the scaffold stage.
   *     log2-spaced allocation-lifetime histogram.
   *
   *   `reserved[]`
   *     Forward-compat slot pool.  As of `SNMALLOC_FULL_STATS_VERSION = 2`
   *     (Phase 11.4) the first `SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS`
   *     (== 16) slots carry the log2-bucketed free-chunk histogram of
   *     the `LargeBuddyRange` pools: `reserved[i]` is the count of
   *     currently-free chunks of size `1 << (MIN_CHUNK_BITS + i)` bytes
   *     for `i` in `[0, 15]`.  Slots `reserved[16..]` remain zero and
   *     are still available for future additive extensions; the offsets
   *     of every previously-defined field above stay fixed.
   */
  struct snmalloc_full_stats
  {
    /* Wire-format version (always populated). */
    uint32_t version;
    /* Explicit padding so the following uint64_t fields are naturally
     * aligned regardless of compiler/platform.  The layout below is the
     * canonical wire form: any future change to this header must
     * preserve the offsets of the already-defined fields. */
    uint32_t _pad0;

    /* Live OS-level reservation (Phase 4 / Phase 7, delegated to
     * StatsRange). */
    uint64_t bytes_in_use;
    uint64_t peak_bytes_in_use;

    /* Phase 9.4 -- mapping / commit accounting. */
    uint64_t bytes_mapped;
    uint64_t bytes_committed;
    uint64_t bytes_decommitted_to_os;

    /* Phase 9.2 -- hot-path counters. */
    uint64_t fast_path_allocs;
    uint64_t slow_path_allocs;
    uint64_t fast_path_deallocs;
    uint64_t remote_deallocs;
    uint64_t message_queue_drains;
    uint64_t cross_thread_messages_received;

    /* Phase 9.3 -- per-size-class histograms. */
    uint64_t total_live_bytes_by_class[SNMALLOC_FULL_STATS_SIZECLASS_SLOTS];
    uint64_t total_live_count_by_class[SNMALLOC_FULL_STATS_SIZECLASS_SLOTS];
    uint64_t cumulative_alloc_by_class[SNMALLOC_FULL_STATS_SIZECLASS_SLOTS];
    uint64_t cumulative_dealloc_by_class[SNMALLOC_FULL_STATS_SIZECLASS_SLOTS];

    /* Phase 9.5 -- log2-spaced allocation-lifetime distribution. */
    uint64_t lifetime_buckets_ns[SNMALLOC_FULL_STATS_LIFETIME_BUCKETS];

    /* Forward-compat reserve pool. */
    uint64_t reserved[SNMALLOC_FULL_STATS_RESERVED_SLOTS];
  };

  /**
   * Populate `*out` with a coherent snapshot of allocator telemetry.
   *
   * The function zero-initialises `*out` first (so unimplemented fields
   * read as zero on every platform), then fills in `version`,
   * `bytes_in_use`, and `peak_bytes_in_use`.  The remaining fields will
   * be wired up by the Phase 9 wave-2 tickets.
   *
   * `out` must be non-NULL.  No allocator state is mutated -- the call
   * is a pure read.  Safe to call from any thread at any point in the
   * process lifetime (the underlying `StatsRange` counters are atomic).
   */
  SNMALLOC_EXPORT void snmalloc_get_full_stats(struct snmalloc_full_stats* out);

#ifdef __cplusplus
} // extern "C"
#endif
