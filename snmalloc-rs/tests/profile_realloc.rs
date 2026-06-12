//! Integration tests for the realloc event hook (ticket 86aj0hk9y).
//!
//! Exercises the Rust-side view of `record_realloc` on the in-place
//! realloc fast path:
//!
//! - A streaming session running while we drive a workload of growing
//!   in-place reallocs must observe at least one
//!   [`snmalloc_rs::streaming::EventKind::Resize`] event whose
//!   `requested_size` reflects the post-resize size.
//!
//! - Snapshot mode never produces a `Resize`-tagged sample: the
//!   persisted slot is updated in place but its `kind` byte stays
//!   `Alloc` (see `record_realloc` in `src/snmalloc/profile/record.h`).
//!
//! Both tests gate on the `profiling` Cargo feature; with the feature
//! off the FFI is a no-op and the test trivially passes.

#![cfg(feature = "profiling")]

use snmalloc_rs::streaming::EventKind;
use snmalloc_rs::{ProfilingSession, SnMalloc};
use std::alloc::{GlobalAlloc, Layout};
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

/// Cargo runs integration tests on multiple threads; the streaming
/// session is process-global and at most one can be active at a time.
/// Serialise through a process-local mutex.
fn session_lock() -> &'static Mutex<()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
}

/// In-place realloc broadcasts at least one `EventKind::Resize` event.
///
/// Strategy: set sampling rate to 1 byte so every alloc is sampled,
/// start a streaming session, then drive a workload of allocations and
/// reallocs through the snmalloc allocator directly (via `GlobalAlloc`
/// + the `realloc` method).  The `realloc` method funnels through
/// `sn_rust_realloc`, which uses the same in-place fast path that
/// `snmalloc::libc::realloc` does -- both of which now invoke the
/// `record_realloc` hook (ticket 86aj0hk9y).
///
/// We use the `SnMalloc` adapter directly rather than relying on the
/// global allocator wiring: integration tests are compiled without
/// `#[global_allocator] = SnMalloc`, so `Vec::reserve` would not route
/// through snmalloc.
#[test]
fn streaming_sees_resize_event_on_inplace_realloc() {
    let _guard = session_lock().lock().unwrap_or_else(|e| e.into_inner());

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        // Profiling feature is off at the C build level; bail safely.
        return;
    }
    let saved_rate = a.sampling_rate();
    a.set_sampling_rate(1);

    let resize_count = Arc::new(AtomicU64::new(0));
    let alloc_count = Arc::new(AtomicU64::new(0));
    let last_resize_req = Arc::new(AtomicUsize::new(0));
    let last_resize_alloc = Arc::new(AtomicUsize::new(0));

    let rc = Arc::clone(&resize_count);
    let ac = Arc::clone(&alloc_count);
    let lrq = Arc::clone(&last_resize_req);
    let lra = Arc::clone(&last_resize_alloc);

    let session = ProfilingSession::start(move |sample| {
        match sample.kind() {
            EventKind::Resize => {
                rc.fetch_add(1, Ordering::Relaxed);
                lrq.store(sample.requested_size(), Ordering::Relaxed);
                lra.store(sample.allocated_size(), Ordering::Relaxed);
            }
            EventKind::Alloc => {
                ac.fetch_add(1, Ordering::Relaxed);
            }
        }
    })
    .expect("first ProfilingSession::start must succeed");

    // Drive a workload of explicit alloc/realloc pairs through the
    // snmalloc allocator surface.  Each realloc to a size in the same
    // sizeclass takes the in-place fast path and should broadcast a
    // Resize event.
    //
    // Repeat enough times to (a) drain any large per-thread countdown
    // left over from a previous test and (b) get enough Poisson-fired
    // samples that at least one Resize broadcast lands.
    const ITERS: usize = 4096;
    const BASE_SIZE: usize = 100; // rounds up to the 128-byte sizeclass
    const GROW_SIZE: usize = 101; // still rounds up to 128
    let base_layout = Layout::from_size_align(BASE_SIZE, 8).unwrap();
    for _ in 0..ITERS {
        let p = unsafe { a.alloc(base_layout) };
        assert!(!p.is_null());
        // In-place realloc within the same sizeclass.
        let p2 = unsafe { a.realloc(p, base_layout, GROW_SIZE) };
        assert!(!p2.is_null());
        // The grown layout shares the alignment but has the new size.
        let grow_layout = Layout::from_size_align(GROW_SIZE, 8).unwrap();
        unsafe { a.dealloc(p2, grow_layout) };
    }

    drop(session);

    let observed_resize = resize_count.load(Ordering::Relaxed);
    let observed_alloc = alloc_count.load(Ordering::Relaxed);
    let observed_last_req = last_resize_req.load(Ordering::Relaxed);
    let observed_last_alloc = last_resize_alloc.load(Ordering::Relaxed);

    // Restore the saved rate before any assertion failure so the
    // process-global state doesn't leak into other tests.
    a.set_sampling_rate(saved_rate);

    assert!(
        observed_alloc > 0,
        "streaming handler must have seen at least one Alloc broadcast \
         after {ITERS} alloc/realloc cycles at rate=1; got {observed_alloc}"
    );
    assert!(
        observed_resize > 0,
        "streaming handler must have seen at least one Resize broadcast \
         from the in-place realloc fast path after {ITERS} iterations \
         at rate=1; got {observed_resize} (alloc events: {observed_alloc})"
    );
    // The most-recent Resize event must carry the post-resize sizes
    // we drove through `realloc`.
    assert_eq!(
        observed_last_req, GROW_SIZE,
        "Resize broadcast requested_size should match the grow-to value"
    );
    assert!(
        observed_last_alloc >= observed_last_req,
        "Resize allocated_size {observed_last_alloc} must be >= requested_size {observed_last_req}"
    );
}

/// Snapshot mode never observes a `Resize`-tagged sample.  The
/// persisted SampledList slot is updated in place by `record_realloc`,
/// but its `kind` byte stays `Alloc` because the sample's lifecycle
/// did not change -- only its size did.  `BtSample::kind()` therefore
/// always returns `SampleKind::Alloc` for a snapshot.
#[test]
fn snapshot_kind_is_always_alloc() {
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }
    let saved_rate = a.sampling_rate();
    a.set_sampling_rate(1);

    // Drive a small workload through the snmalloc allocator surface
    // so we have live samples + in-place reallocs in the SampledList.
    let layout = Layout::from_size_align(100, 8).unwrap();
    let mut leaked: Vec<*mut u8> = Vec::new();
    for _ in 0..64 {
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null());
        let p2 = unsafe { a.realloc(p, layout, 101) };
        assert!(!p2.is_null());
        leaked.push(p2);
    }

    let snap = a.snapshot();
    for sample in snap.samples() {
        assert_eq!(
            sample.kind(),
            snmalloc_rs::profile::SampleKind::Alloc,
            "snapshot samples must always carry SampleKind::Alloc; \
             saw a Resize-tagged sample which means the persisted \
             slot's kind byte was mis-set by record_realloc"
        );
    }

    // Clean up the leaked buffers.
    let grow_layout = Layout::from_size_align(101, 8).unwrap();
    for p in leaked {
        unsafe { a.dealloc(p, grow_layout) };
    }

    a.set_sampling_rate(saved_rate);
}
