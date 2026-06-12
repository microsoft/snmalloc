//! Integration test for the Phase 9.2 per-thread frontend cache stats
//! (ClickUp 86aj0tr1e).
//!
//! Exercises the alloc / dealloc counter wiring exposed via
//! `SnMalloc::full_stats()`:
//!
//!   * `fast_path_allocs` / `slow_path_allocs` -- bumped on the
//!     respective branches of `Allocator::small_alloc`.
//!   * `fast_path_deallocs` -- bumped on the local-owner branch of
//!     `Allocator::dealloc`.
//!   * `remote_deallocs` -- bumped on the cross-allocator branch of
//!     `Allocator::dealloc`.
//!   * `cross_thread_messages_received` -- bumped per message
//!     dequeued from another thread's post.
//!   * `message_queue_drains` -- bumped once per
//!     `handle_message_queue_slow` invocation.
//!
//! The test mirrors the C++-side `src/test/func/fast_path_counters`
//! test: drive a single-thread burst of allocations and frees to
//! grow the fast-path counters, then spawn a worker that performs
//! cross-thread frees to grow `remote_deallocs` and (after the main
//! thread drains its message queue) the receive-side counters.
//!
//! Gated behind `#[cfg(feature = "stats")]` because `full_stats()`
//! is itself feature-gated -- the same compile-time gate the Phase
//! 9.1 scaffold and `full_stats.rs` test use.  The C++-side counter
//! sites compile away to zero increments when `SNMALLOC_STATS=OFF`,
//! so this test only meaningfully exercises wired-up counters when
//! the feature is on.

// Phase 11.6 -- this test exercises only FrontendStats fields,
// which the BASIC tier maintains.  Run under `stats-basic` (or, by
// implication, `stats-full` / legacy `stats`); skipped otherwise.
#![cfg(feature = "stats-basic")]

use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;

/// Number of cross-thread frees driven by the worker.  Each free
/// targets a 512-byte object, so the total (64 KiB) is comfortably
/// large enough to saturate the worker's per-thread remote-dealloc
/// cache (`REMOTE_CACHE`, typically 16-128 KiB).  Saturating the
/// cache forces an in-thread `post()` rather than waiting for the
/// teardown flush -- which makes the cross-thread message visible
/// to the main thread immediately, regardless of platform-specific
/// thread-local destructor ordering.
const K: usize = 128;
const CROSS_OBJ_SIZE: usize = 512;

#[test]
fn fast_path_alloc_counter_grows() {
    let alloc = SnMalloc::new();
    let before = SnMalloc::full_stats();

    // 1000 small allocations of one sizeclass.  The first one or two
    // may take the slow path while the slab opens; the rest should
    // hit the fast free list and bump `fast_path_allocs`.
    const N: usize = 1000;
    let layout = Layout::from_size_align(32, 16).unwrap();
    let mut ptrs = Vec::with_capacity(N);
    for _ in 0..N {
        let p = unsafe { alloc.alloc(layout) };
        assert!(!p.is_null(), "alloc must succeed");
        ptrs.push(p);
    }

    let after_alloc = SnMalloc::full_stats();
    let alloc_delta = after_alloc.fast_path_allocs - before.fast_path_allocs;
    // Each slow refill consumes one "missed fast-path" slot, so for
    // 1000 single-sizeclass allocs we observe ~998-999.  Lower-bound
    // at N-10 to absorb the (very rare) case of multiple refills.
    assert!(
        alloc_delta >= (N as u64) - 10,
        "fast_path_allocs delta (={}) must rise by at least {} after {} \
         small allocations",
        alloc_delta,
        (N as u64) - 10,
        N
    );

    // Slow-path counter must rise too (at least the first slab open).
    assert!(
        after_alloc.slow_path_allocs > before.slow_path_allocs,
        "slow_path_allocs must rise across slab opens \
         (before={}, after={})",
        before.slow_path_allocs,
        after_alloc.slow_path_allocs,
    );

    // Free everything on the same thread; the fast-dealloc counter
    // should also rise by ~N.
    for p in ptrs.drain(..) {
        unsafe { alloc.dealloc(p, layout) };
    }
    let after_dealloc = SnMalloc::full_stats();
    let dealloc_delta =
        after_dealloc.fast_path_deallocs - after_alloc.fast_path_deallocs;
    assert!(
        dealloc_delta >= (N as u64) - 10,
        "fast_path_deallocs delta (={}) must rise by at least {} after {} \
         same-thread frees",
        dealloc_delta,
        (N as u64) - 10,
        N
    );
}

#[test]
fn cross_thread_messages_grow() {
    // Pre-allocate K objects on the main thread.  These will be
    // freed by the worker so each free takes the remote branch of
    // `Allocator::dealloc`.  Using a moderately-sized payload (512
    // bytes per object, K=128 -> 64 KiB total) is large enough to
    // exhaust the worker's remote-dealloc cache and force at least
    // one in-thread `post()` mid-thread, which puts the
    // cross-thread message into the main thread's queue
    // deterministically.
    let main_alloc = SnMalloc::new();
    let before = SnMalloc::full_stats();

    let layout = Layout::from_size_align(CROSS_OBJ_SIZE, 16).unwrap();
    let mut ptrs: Vec<usize> = Vec::with_capacity(K);
    for _ in 0..K {
        let p = unsafe { main_alloc.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p as usize);
    }
    // SAFETY: We're going to transfer ownership of these raw pointers
    // to the worker thread.  Wrapping as `usize` strips the
    // `*mut u8`'s `!Send` so we can move the Vec across threads;
    // the worker reconstructs the pointers locally.
    let ptrs_for_worker = Arc::new(ptrs);
    let go = Arc::new(AtomicBool::new(false));
    let done_count = Arc::new(AtomicUsize::new(0));

    let ptrs_w = Arc::clone(&ptrs_for_worker);
    let go_w = Arc::clone(&go);
    let done_w = Arc::clone(&done_count);

    let worker = thread::spawn(move || {
        let alloc = SnMalloc::new();
        while !go_w.load(Ordering::Acquire) {
            std::hint::spin_loop();
        }
        for &addr in ptrs_w.iter() {
            unsafe { alloc.dealloc(addr as *mut u8, layout) };
        }
        done_w.store(K, Ordering::Release);
    });

    go.store(true, Ordering::Release);
    worker.join().expect("worker join");
    assert_eq!(done_count.load(Ordering::Acquire), K);

    // Worker has exited; its allocator's per-thread stats have been
    // drained into the process-global aggregator (see
    // `ThreadAlloc::teardown` + `Allocator::drain_stats_to_global`).
    // The `remote_deallocs` counter should have risen by at least K.
    let after_worker = SnMalloc::full_stats();
    let remote_delta =
        after_worker.remote_deallocs - before.remote_deallocs;
    assert!(
        remote_delta >= K as u64,
        "remote_deallocs delta (={}) must rise by at least K={} after \
         {} cross-thread frees",
        remote_delta,
        K,
        K,
    );

    // Drive the main thread to drain its incoming message queue.
    // Each fresh sizeclass starts with an empty fast list and routes
    // through `handle_message_queue`, which calls
    // `handle_message_queue_slow` (bumps `message_queue_drains`) and
    // walks the queue (bumps `cross_thread_messages_received`).
    for rep in 0..256 {
        let sz = 16 + (rep * 17) % 256;
        let layout_i = Layout::from_size_align(sz, 16).unwrap();
        let p = unsafe { main_alloc.alloc(layout_i) };
        if !p.is_null() {
            unsafe { main_alloc.dealloc(p, layout_i) };
        }
    }

    let after_drain = SnMalloc::full_stats();
    let msgs_delta = after_drain.cross_thread_messages_received
        - before.cross_thread_messages_received;
    let drains_delta = after_drain.message_queue_drains
        - before.message_queue_drains;
    assert!(
        msgs_delta >= 1,
        "cross_thread_messages_received delta (={}) must rise by at \
         least 1 after worker posts and main drains",
        msgs_delta,
    );
    assert!(
        drains_delta >= 1,
        "message_queue_drains delta (={}) must rise by at least 1 \
         after main enters the queue-drain slow path",
        drains_delta,
    );
}
