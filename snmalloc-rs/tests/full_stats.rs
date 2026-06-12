//! Integration test for the Phase 9.1 `FullAllocStats` scaffold.
//!
//! The Rust-side `SnMalloc::full_stats()` getter delegates to the C
//! ABI `snmalloc_get_full_stats` (declared in
//! `src/snmalloc/global/stats_export.h` and implemented in
//! `src/snmalloc/override/stats_export.cc`).  At the scaffold stage
//! only `version`, `bytes_in_use`, and `peak_bytes_in_use` carry
//! meaningful values; every other field is zero and will be populated
//! by the Phase 9 wave-2 tickets.
//!
//! This test exists in its own integration-test binary (separate from
//! `memory_stats.rs`) for the same reason that test does: the
//! underlying counters are process-global, so we want isolation from
//! other allocating tests that cargo runs in parallel threads of the
//! same binary.
//!
//! Gated behind `#[cfg(feature = "stats")]` because `full_stats()` is
//! itself feature-gated -- without the `stats` feature the symbol does
//! not exist (intentional compile-time gate, not a runtime-zero stub).

#![cfg(feature = "stats")]

use snmalloc_rs::{FullAllocStats, SnMalloc, SNMALLOC_FULL_STATS_VERSION};
use std::alloc::{GlobalAlloc, Layout};

/// Helper: confirm every field that the scaffold has *not* wired up
/// is zero.  Keeping this check in one place makes it obvious which
/// fields are deliberately left for wave-2 tickets to populate.
fn assert_all_unimplemented_fields_are_zero(s: &FullAllocStats) {
    // Phase 9.4 fields are now wired and asserted positively below in
    // the dedicated test; they are intentionally NOT checked for zero
    // here.

    // Phase 9.2 -- hot-path counters.
    assert_eq!(s.fast_path_allocs, 0, "9.2: fast_path_allocs not yet wired");
    assert_eq!(s.slow_path_allocs, 0, "9.2: slow_path_allocs not yet wired");
    assert_eq!(
        s.fast_path_deallocs, 0,
        "9.2: fast_path_deallocs not yet wired"
    );
    assert_eq!(s.remote_deallocs, 0, "9.2: remote_deallocs not yet wired");
    assert_eq!(
        s.message_queue_drains, 0,
        "9.2: message_queue_drains not yet wired"
    );
    assert_eq!(
        s.cross_thread_messages_received, 0,
        "9.2: cross_thread_messages_received not yet wired"
    );

    // Phase 9.3 -- per-size-class histograms.
    assert!(
        s.total_live_bytes_by_class.iter().all(|&b| b == 0),
        "9.3: total_live_bytes_by_class not yet wired"
    );
    assert!(
        s.total_live_count_by_class.iter().all(|&c| c == 0),
        "9.3: total_live_count_by_class not yet wired"
    );
    assert!(
        s.cumulative_alloc_by_class.iter().all(|&c| c == 0),
        "9.3: cumulative_alloc_by_class not yet wired"
    );
    assert!(
        s.cumulative_dealloc_by_class.iter().all(|&c| c == 0),
        "9.3: cumulative_dealloc_by_class not yet wired"
    );

    // Phase 9.5 -- allocation-lifetime histogram.
    assert!(
        s.lifetime_buckets_ns.iter().all(|&b| b == 0),
        "9.5: lifetime_buckets_ns not yet wired"
    );
}

#[test]
fn full_stats_version_is_populated() {
    let stats = SnMalloc::full_stats();
    assert_eq!(
        stats.version, SNMALLOC_FULL_STATS_VERSION,
        "version must match SNMALLOC_FULL_STATS_VERSION"
    );
}

#[test]
fn full_stats_bytes_in_use_grows_with_live_allocation() {
    // `SnMalloc` is not the process-wide global allocator in this
    // test binary (cargo's default test runner uses the system
    // allocator), so we must drive it explicitly through the
    // `GlobalAlloc` trait.  This is the same pattern that the
    // adjacent `memory_stats.rs` test uses for the legacy
    // `memory_stats()` getter.
    let alloc = SnMalloc::new();
    let before = SnMalloc::full_stats();

    let layout = Layout::from_size_align(1 << 20, 64).unwrap();
    let ptr = unsafe { alloc.alloc(layout) };
    assert!(!ptr.is_null(), "1 MiB allocation must not return null");

    let during = SnMalloc::full_stats();

    assert!(
        during.bytes_in_use > 0,
        "bytes_in_use must be non-zero with a 1 MiB live allocation, \
         got {}",
        during.bytes_in_use
    );
    assert!(
        during.bytes_in_use >= before.bytes_in_use,
        "bytes_in_use must not regress after a fresh allocation \
         (before = {}, during = {})",
        before.bytes_in_use,
        during.bytes_in_use
    );
    assert!(
        during.peak_bytes_in_use >= during.bytes_in_use,
        "peak_bytes_in_use ({}) must be >= bytes_in_use ({})",
        during.peak_bytes_in_use,
        during.bytes_in_use
    );

    // The whole point of the scaffold: every wave-2 field must be
    // zero today.  When a wave-2 ticket lands, the corresponding
    // assertion here will start failing and signal that the test
    // needs to evolve along with the new field.
    assert_all_unimplemented_fields_are_zero(&during);

    // Release the buffer back to the allocator.
    unsafe { alloc.dealloc(ptr, layout) };
}

#[test]
fn full_stats_backend_frag_invariants() {
    // Phase 9.4 -- `bytes_mapped` / `bytes_committed` /
    // `bytes_decommitted_to_os` must satisfy the documented
    // invariants once an allocation has driven traffic through the
    // CommitRange.
    let alloc = SnMalloc::new();

    // Push enough memory through the backend that we exercise the
    // commit path -- a 1 MiB allocation forces the local cache to
    // refill from the global range, which is where the
    // `notify_using` hook lives.  Multiple allocations make the
    // counter non-zero even when the local cache was warm.
    let layout = Layout::from_size_align(1 << 20, 64).unwrap();
    let p1 = unsafe { alloc.alloc(layout) };
    let p2 = unsafe { alloc.alloc(layout) };
    assert!(!p1.is_null() && !p2.is_null());

    let snap = SnMalloc::full_stats();

    // The cumulative commit counter must be positive after we've
    // forced at least one parent-range refill.
    assert!(
        snap.bytes_committed > 0,
        "bytes_committed must be > 0 after live allocations; got {}",
        snap.bytes_committed
    );

    // Live committed bytes can never exceed live mapped bytes -- the
    // commit happens on top of an existing mapping.  (`bytes_mapped`
    // is sourced from `StatsRange::get_current_usage`, which is the
    // live OS reservation.)
    assert!(
        snap.bytes_committed <= snap.bytes_mapped,
        "bytes_committed ({}) must be <= bytes_mapped ({})",
        snap.bytes_committed,
        snap.bytes_mapped
    );

    unsafe { alloc.dealloc(p1, layout) };
    unsafe { alloc.dealloc(p2, layout) };

    // After freeing, bytes_committed may or may not have dropped
    // (depends on whether the local cache decided to release back to
    // the parent range), but the cumulative decommit counter is
    // non-decreasing and the version is unchanged.
    let after = SnMalloc::full_stats();
    assert!(
        after.bytes_decommitted_to_os >= snap.bytes_decommitted_to_os,
        "bytes_decommitted_to_os must be monotone non-decreasing \
         (snap = {}, after = {})",
        snap.bytes_decommitted_to_os,
        after.bytes_decommitted_to_os
    );
    assert_eq!(after.version, SNMALLOC_FULL_STATS_VERSION);
}

#[test]
fn full_stats_peak_is_monotone_after_dealloc() {
    let alloc = SnMalloc::new();
    let before = SnMalloc::full_stats();

    let layout = Layout::from_size_align(1 << 20, 64).unwrap();
    let ptr = unsafe { alloc.alloc(layout) };
    assert!(!ptr.is_null());
    // Drop the live allocation back to the allocator's local cache.
    // StatsRange semantics mean `bytes_in_use` may fall back down,
    // but `peak_bytes_in_use` must not regress.
    unsafe { alloc.dealloc(ptr, layout) };

    let after = SnMalloc::full_stats();
    assert!(
        after.peak_bytes_in_use >= before.peak_bytes_in_use,
        "peak_bytes_in_use must be monotone non-decreasing across a \
         dealloc (before = {}, after = {})",
        before.peak_bytes_in_use,
        after.peak_bytes_in_use
    );
}
