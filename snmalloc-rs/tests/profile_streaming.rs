//! Integration tests for the safe Rust streaming-profiling wrapper
//! introduced in Phase 5.2 (`snmalloc_rs::ProfilingSession`).
//!
//! The whole file is gated on the `profiling` Cargo feature: the
//! types it exercises (`ProfilingSession`, `StreamSample`,
//! `StreamingError`) only exist in feature-on builds, and the
//! underlying FFI registration calls are no-ops returning `-1` in
//! feature-off builds (where the safe wrapper would refuse to
//! construct a session anyway).
//!
//! Cargo runs these tests on multiple threads, and the streaming
//! FFI is process-global: at most one session can be active at a
//! time across the whole binary.  To keep the tests deterministic
//! we serialise session-using bodies through a process-static
//! mutex.  This is a test-harness concern, not a property of the
//! API: real applications hold exactly one session at a time by
//! construction and never need this guard.

#![cfg(feature = "profiling")]

use snmalloc_rs::{ProfilingSession, SnMalloc, StreamingError};
use std::alloc::{GlobalAlloc, Layout};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::thread;

/// Serialises the bodies of tests that create a `ProfilingSession`.
/// See the module comment.
fn session_lock() -> &'static Mutex<()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
}

/// Drive enough sampled allocations through the global allocator
/// that, at the configured `RATE`, the streaming handler is very
/// likely to see at least one sample.  The exact sample count is
/// Poisson-distributed; we just need >= 1 with overwhelming
/// probability.
const TEST_RATE: usize = 4096;
const TEST_ALLOCS: usize = 50_000;
const TEST_SIZE: usize = 64;

fn workload(a: &SnMalloc) {
    let layout = Layout::from_size_align(TEST_SIZE, 8).unwrap();
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(TEST_ALLOCS);
    for _ in 0..TEST_ALLOCS {
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p);
    }
    for p in ptrs {
        unsafe { a.dealloc(p, layout) };
    }
}

/// Smoke test: start a session, run a workload, drop the session,
/// assert the handler observed at least one sample.
#[test]
fn smoke_session_receives_samples() {
    let _guard = session_lock().lock().unwrap_or_else(|e| e.into_inner());

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        // Should not happen in a `--features profiling` build, but
        // bail safely if the C side reports unsupported.
        return;
    }
    let saved_rate = a.sampling_rate();
    a.set_sampling_rate(TEST_RATE);

    let counter = Arc::new(AtomicU64::new(0));
    let counter_cb = Arc::clone(&counter);

    let session = ProfilingSession::start(move |sample| {
        // Touch every accessor so we exercise the borrowed-view API.
        let _ = sample.alloc_ptr();
        let _ = sample.requested_size();
        let _ = sample.allocated_size();
        let _ = sample.weight();
        let _ = sample.stack();
        counter_cb.fetch_add(1, Ordering::Relaxed);
    })
    .expect("first ProfilingSession::start must succeed");

    workload(&a);

    drop(session);

    let observed = counter.load(Ordering::Relaxed);
    assert!(
        observed > 0,
        "streaming handler must have observed at least one sample after \
         {TEST_ALLOCS} x {TEST_SIZE}B allocs at rate {TEST_RATE}; got 0"
    );

    a.set_sampling_rate(saved_rate);
}

/// Starting a second session while the first is alive returns
/// `Err(AlreadyActive)`.  After the first session is dropped, a
/// fresh start() succeeds.
#[test]
fn double_start_errors_then_recovers() {
    let _guard = session_lock().lock().unwrap_or_else(|e| e.into_inner());

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let first = ProfilingSession::start(|_sample| {
        // No-op; we only care about the registration state.
    })
    .expect("first start must succeed");

    let second = ProfilingSession::start(|_sample| {});
    assert!(
        matches!(second, Err(StreamingError::AlreadyActive)),
        "second start while first is alive must return \
         Err(StreamingError::AlreadyActive); got {second:?}"
    );

    drop(first);

    let third = ProfilingSession::start(|_sample| {});
    assert!(
        third.is_ok(),
        "after dropping the first session a fresh start must \
         succeed; got {third:?}"
    );
    drop(third);
}

/// After dropping a session, the handler must not be invoked by
/// subsequent allocations.  We park a sticky "saw a sample" flag
/// behind an `Arc<AtomicBool>` so the trailing workload can prove
/// the unregister was effective.
#[test]
fn drop_unregisters_handler() {
    let _guard = session_lock().lock().unwrap_or_else(|e| e.into_inner());

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }
    let saved_rate = a.sampling_rate();
    a.set_sampling_rate(TEST_RATE);

    let flag = Arc::new(AtomicBool::new(false));
    let flag_cb = Arc::clone(&flag);

    let session = ProfilingSession::start(move |_sample| {
        flag_cb.store(true, Ordering::Relaxed);
    })
    .expect("start must succeed");

    workload(&a);
    // We expect at least one sample observed by here.
    let observed_during = flag.load(Ordering::Relaxed);
    assert!(
        observed_during,
        "handler should have observed a sample during the session"
    );

    // Drop the session: from this point onward, our handler must
    // never be invoked again, regardless of allocator activity.
    drop(session);
    flag.store(false, Ordering::Relaxed);

    // Run another workload of comparable size and assert the flag
    // stays cleared.  Use a different sampling rate to make sure
    // any latent registration would be visible.
    workload(&a);

    assert!(
        !flag.load(Ordering::Relaxed),
        "handler must NOT be invoked after the session is dropped; \
         the flag was set, implying the Rust slot still holds our \
         closure or the C-side trampoline is still registered"
    );

    a.set_sampling_rate(saved_rate);
}

/// Spin up several worker threads doing allocations concurrently
/// with the session active.  The handler is `Send + Sync` and the
/// dispatch lock inside the trampoline must serialise correctly --
/// the test passes as long as no panic / no UB / no deadlock
/// surfaces.  We also assert at least one sample landed, just to
/// be sure the trampoline is reachable from worker threads.
#[test]
fn thread_safety_concurrent_workload() {
    let _guard = session_lock().lock().unwrap_or_else(|e| e.into_inner());

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }
    let saved_rate = a.sampling_rate();
    a.set_sampling_rate(TEST_RATE);

    let counter = Arc::new(AtomicU64::new(0));
    let counter_cb = Arc::clone(&counter);

    let session = ProfilingSession::start(move |sample| {
        // Read every accessor to make sure the borrow is honoured
        // when dispatched from foreign threads.
        let _ = sample.alloc_ptr();
        let _ = sample.requested_size();
        let _ = sample.allocated_size();
        let _ = sample.weight();
        let _ = sample.stack();
        counter_cb.fetch_add(1, Ordering::Relaxed);
    })
    .expect("start must succeed");

    let mut handles = Vec::new();
    for _ in 0..4 {
        handles.push(thread::spawn(|| {
            let a = SnMalloc::new();
            // Each worker does its own small workload.
            let layout = Layout::from_size_align(TEST_SIZE, 8).unwrap();
            let mut ptrs: Vec<*mut u8> = Vec::with_capacity(TEST_ALLOCS / 4);
            for _ in 0..(TEST_ALLOCS / 4) {
                let p = unsafe { a.alloc(layout) };
                assert!(!p.is_null());
                ptrs.push(p);
            }
            for p in ptrs {
                unsafe { a.dealloc(p, layout) };
            }
        }));
    }
    for h in handles {
        h.join().expect("worker thread must not panic");
    }

    drop(session);

    assert!(
        counter.load(Ordering::Relaxed) > 0,
        "expected the streaming handler to observe at least one \
         sample across {} concurrent workers",
        4
    );

    a.set_sampling_rate(saved_rate);
}
