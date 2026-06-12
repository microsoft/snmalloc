//! Integration test for the Phase 9.3 per-size-class histogram
//! (ClickUp 86aj0tr4p).
//!
//! Exercises the four per-class arrays in `FullAllocStats`:
//!
//!   * `cumulative_alloc_by_class[]` -- monotone, bumped on every
//!     small alloc that resolves to a given sizeclass on the
//!     producing thread.
//!   * `cumulative_dealloc_by_class[]` -- monotone, bumped on every
//!     small dealloc on the freeing thread (which may or may not
//!     be the owning thread for cross-thread frees).
//!   * `total_live_count_by_class[]` -- net live object count per
//!     class.  Live counts are decremented on the owning thread,
//!     either on the local-fast-path dealloc or on the message-
//!     queue drain path for cross-thread frees.
//!   * `total_live_bytes_by_class[]` -- net live byte total per
//!     class.
//!
//! The test pins a single sizeclass by repeatedly allocating the
//! same byte size, then identifies which slot the allocator chose
//! by scanning for the first non-zero `cumulative_alloc_by_class[]`
//! delta.  This avoids hard-coding `sizeclass_to_size(1)` in the
//! test, which would couple the test to snmalloc's internal class
//! table.
//!
//! Gated behind `#[cfg(feature = "stats")]` because `full_stats()`
//! is itself feature-gated.  Without the `stats` feature the
//! counters compile away to no-ops on the C++ side, and the symbol
//! does not exist on the Rust side.

// Phase 11.6 -- the per-size-class histogram is FULL-tier only.
// Under `stats-basic` the `*_by_class[]` arrays are all-zero by
// design (the BASIC tier deliberately skips the per-class hot-path
// stores to stay inside the <= 2% overhead budget), so this test
// would not have meaningful deltas to assert against.  Gated to
// `stats-full` accordingly.
#![cfg(feature = "stats-full")]

use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};

/// Number of objects to allocate of the pinned size.  Chosen large
/// enough that the per-class signal dominates any background
/// per-class traffic from other concurrently-running cargo tests
/// inside the same binary.
const N: usize = 100;

/// Size of each pinned allocation.  32 bytes is small enough to
/// land squarely on a small sizeclass on every reasonable snmalloc
/// configuration, and large enough to skip the very-smallest class
/// where library bookkeeping may have already left traffic.
const ALLOC_SIZE: usize = 32;

/// Find the sizeclass index `i` for which `cumulative_alloc_by_class[i]`
/// rose the most between `before` and `after`.  Returns `Some((i,
/// delta))` if a non-zero delta exists, or `None` otherwise.
fn dominant_class(
    before: &[u64],
    after: &[u64],
) -> Option<(usize, u64)> {
    let mut best: Option<(usize, u64)> = None;
    for (i, (b, a)) in before.iter().zip(after.iter()).enumerate() {
        let delta = a.saturating_sub(*b);
        if delta == 0 {
            continue;
        }
        match best {
            None => best = Some((i, delta)),
            Some((_, d)) if delta > d => best = Some((i, delta)),
            _ => {}
        }
    }
    best
}

#[test]
fn cumulative_alloc_per_class_rises() {
    let alloc = SnMalloc::new();
    let before = SnMalloc::full_stats();

    let layout = Layout::from_size_align(ALLOC_SIZE, 16).unwrap();
    let mut ptrs = Vec::with_capacity(N);
    for _ in 0..N {
        let p = unsafe { alloc.alloc(layout) };
        assert!(!p.is_null(), "alloc must succeed");
        ptrs.push(p);
    }

    let after = SnMalloc::full_stats();

    // Identify the chosen sizeclass via the cumulative_alloc delta.
    let (sc, alloc_delta) = dominant_class(
        &before.cumulative_alloc_by_class,
        &after.cumulative_alloc_by_class,
    )
    .expect(
        "at least one cumulative_alloc_by_class slot must rise after \
         100 same-size allocations",
    );

    assert!(
        alloc_delta >= N as u64,
        "cumulative_alloc_by_class[{}] delta (={}) must rise by at \
         least N={} after {} allocations of size {}",
        sc,
        alloc_delta,
        N,
        N,
        ALLOC_SIZE,
    );

    // Live counters must mirror cumulative for the same class --
    // we haven't freed anything yet.
    let live_count_delta = after.total_live_count_by_class[sc]
        - before.total_live_count_by_class[sc];
    assert!(
        live_count_delta >= N as u64,
        "total_live_count_by_class[{}] delta (={}) must rise by at \
         least N={} after {} allocations (no frees yet)",
        sc,
        live_count_delta,
        N,
        N,
    );

    let live_bytes_delta = after.total_live_bytes_by_class[sc]
        - before.total_live_bytes_by_class[sc];
    // The chosen sizeclass's per-object size is `live_bytes_delta /
    // live_count_delta`; check the invariant that every live byte
    // belongs to some live object.  Using `>=` instead of `==`
    // because pre-existing live objects of the same class are
    // included in the "before" baseline.
    assert!(
        live_bytes_delta >= (live_count_delta) * ALLOC_SIZE as u64,
        "total_live_bytes_by_class[{}] delta (={}) must be >= \
         live_count_delta ({}) * ALLOC_SIZE ({})",
        sc,
        live_bytes_delta,
        live_count_delta,
        ALLOC_SIZE,
    );

    // Free everything; live counters must drop, cumulative
    // counters must stay monotone.
    for p in ptrs.drain(..) {
        unsafe { alloc.dealloc(p, layout) };
    }

    let post_free = SnMalloc::full_stats();

    // cumulative_alloc never regresses.
    assert!(
        post_free.cumulative_alloc_by_class[sc]
            >= after.cumulative_alloc_by_class[sc],
        "cumulative_alloc_by_class[{}] is monotone (after={}, \
         post_free={})",
        sc,
        after.cumulative_alloc_by_class[sc],
        post_free.cumulative_alloc_by_class[sc],
    );

    // cumulative_dealloc must have risen by at least N on the same
    // class (the frees happened on the same thread, so this thread
    // owns both the alloc and the dealloc bookkeeping).
    let dealloc_delta = post_free.cumulative_dealloc_by_class[sc]
        - before.cumulative_dealloc_by_class[sc];
    assert!(
        dealloc_delta >= N as u64,
        "cumulative_dealloc_by_class[{}] delta (={}) must rise by \
         at least N={} after {} frees on the same thread",
        sc,
        dealloc_delta,
        N,
        N,
    );

    // Live count must drop after the frees (down to at most the
    // baseline "before" value -- there may be live objects from
    // other tests, but our N contribution must have unwound).
    assert!(
        post_free.total_live_count_by_class[sc]
            <= after.total_live_count_by_class[sc],
        "total_live_count_by_class[{}] must not rise after frees \
         (after={}, post_free={})",
        sc,
        after.total_live_count_by_class[sc],
        post_free.total_live_count_by_class[sc],
    );

    // Net live drop must be at least N.
    let live_drop = after.total_live_count_by_class[sc]
        - post_free.total_live_count_by_class[sc];
    assert!(
        live_drop >= N as u64,
        "total_live_count_by_class[{}] must drop by at least N={} \
         after {} same-thread frees (after={}, post_free={})",
        sc,
        N,
        N,
        after.total_live_count_by_class[sc],
        post_free.total_live_count_by_class[sc],
    );
}

#[test]
fn cumulative_monotone_invariant_holds() {
    // For every small-sizeclass slot, `cumulative_alloc` must be
    // >= `cumulative_dealloc` -- you can never free more objects
    // than were ever allocated.  This is the strong structural
    // invariant that the per-class histogram must satisfy at every
    // observable instant, even under cross-thread free traffic
    // (where the alloc-side and dealloc-side bookkeeping happen
    // on different per-thread blocks).
    //
    // We deliberately do NOT assert
    // `live_count == cumulative_alloc - cumulative_dealloc` here:
    // the snapshot walks per-thread blocks sequentially without
    // synchronisation, so under concurrent traffic from other
    // tests the three numbers may be read at slightly different
    // instants and the equality may not hold for a single
    // snapshot.  The dedicated single-class test above exercises
    // the live counter behaviour with a controlled allocation
    // pattern instead.
    //
    // Drive a small amount of traffic first so the assertion is
    // not trivially "all zeros".
    let alloc = SnMalloc::new();
    let layout = Layout::from_size_align(48, 16).unwrap();
    let mut ptrs = Vec::with_capacity(16);
    for _ in 0..16 {
        let p = unsafe { alloc.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p);
    }
    for p in ptrs.drain(..8) {
        unsafe { alloc.dealloc(p, layout) };
    }

    let snap = SnMalloc::full_stats();

    for i in 0..snap.cumulative_alloc_by_class.len() {
        let a = snap.cumulative_alloc_by_class[i];
        let d = snap.cumulative_dealloc_by_class[i];

        // cumulative_alloc >= cumulative_dealloc always (cannot
        // free more than was allocated).
        assert!(
            a >= d,
            "class {}: cumulative_alloc ({}) must be >= \
             cumulative_dealloc ({})",
            i,
            a,
            d,
        );
    }

    // Tidy up.
    for p in ptrs.drain(..) {
        unsafe { alloc.dealloc(p, layout) };
    }
}
