//! Follow-up D -- integration tests for the gzip-wrapped pprof
//! encoder ([`HeapProfile::write_pprof_gz`]).
//!
//! Three tests:
//!
//! 1.  `write_pprof_gz_has_gzip_magic` -- on a live snapshot, the
//!     first two emitted bytes are the gzip magic `0x1f 0x8b`, which
//!     lets cloud-profiler ingest endpoints content-sniff the upload
//!     without parsing.
//! 2.  `write_pprof_gz_round_trips_to_write_pprof` -- decoding the
//!     gzipped stream via `flate2::read::GzDecoder` yields byte-for-
//!     byte the same payload as calling [`HeapProfile::write_pprof`]
//!     directly with the same arguments.  This is the structural
//!     equivalence guarantee that lets the new helper drop in to any
//!     existing pprof-driven dashboard.
//! 3.  `write_pprof_gz_empty_snapshot` -- on a default-constructed
//!     [`HeapProfile`], the encoder still produces a *valid* (non-
//!     empty, gzip-magic-prefixed, GzDecoder-parseable) gzip stream
//!     whose decoded payload is the same as `write_pprof` on an empty
//!     snapshot.  Mirrors the totality contract documented on
//!     [`HeapProfile::write_pprof`].
//!
//! Why a real `flate2::read::GzDecoder` round-trip rather than
//! hand-rolling a minimal inflate?  Unlike protobuf -- where a
//! 60-line walker is enough to validate the small subset of fields
//! the encoder emits -- gzip framing has CRC checks, header flags,
//! and an end-of-stream sentinel whose absence we explicitly want to
//! catch.  Using the real decoder protects us from "writer dropped
//! before finish()" footguns that a partial reimplementation would
//! silently let through.

#![cfg(feature = "profiling")]

use snmalloc_rs::{HeapProfile, SnMalloc, Weight};
use std::alloc::{GlobalAlloc, Layout};
use std::io::Read;
use std::sync::{Mutex, MutexGuard, OnceLock};

// =========================================================================
// Workload helpers -- match the shape used in `tests/profile_pprof.rs`.
// Duplicated here (rather than factored into a `mod common`) so that
// each integration-test binary stays self-contained, the way cargo
// expects.
// =========================================================================

const RATE: usize = 512;
const N_ALLOCS: usize = 5_000;
const SIZE: usize = 64;

/// Process-wide mutex so this binary doesn't trip on its sibling
/// `profile_*` workloads running in parallel.  Each integration test
/// compiles to its own binary, so this lock is local to this binary.
fn workload_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

/// Run a workload, take a snapshot, and return it along with a
/// cleanup closure that frees the allocations and restores the
/// previous sampling rate.  Panics if fewer than `min_samples` were
/// captured.
fn run_workload(min_samples: usize) -> (HeapProfile, Box<dyn FnOnce()>) {
    let a = SnMalloc::new();
    let saved = a.sampling_rate();
    a.set_sampling_rate(RATE);

    let layout = Layout::from_size_align(SIZE, 8).expect("valid layout");
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N_ALLOCS);
    for _ in 0..N_ALLOCS {
        // SAFETY: layout is non-zero, every pointer is fed back to
        // dealloc in the cleanup closure.
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null(), "snmalloc alloc returned NULL");
        ptrs.push(p);
    }

    let snap = a.snapshot();
    assert!(
        snap.len() >= min_samples,
        "expected at least {} samples; got {}.  Increase N_ALLOCS or \
         check the SNMALLOC_PROFILE wiring.",
        min_samples,
        snap.len()
    );

    let cleanup = Box::new(move || {
        let a = SnMalloc::new();
        for p in ptrs {
            // SAFETY: each `p` came from `alloc(layout)` above and
            // has not been freed yet.
            unsafe { a.dealloc(p, layout) };
        }
        a.set_sampling_rate(saved);
    });

    (snap, cleanup)
}

// =========================================================================
// Tests
// =========================================================================

/// The encoder must produce a gzip stream -- the very first two bytes
/// are the gzip magic `0x1f 0x8b` per RFC 1952 sec. 2.3.1.
#[test]
fn write_pprof_gz_has_gzip_magic() {
    let _lock = workload_lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        // Belt-and-braces graceful degradation -- mirrors the pattern
        // in `tests/profile_pprof.rs`.
        return;
    }

    let (snap, cleanup) = run_workload(50);

    let mut buf: Vec<u8> = Vec::new();
    snap.write_pprof_gz(&mut buf, Weight::Allocated)
        .expect("Vec<u8> write is infallible");
    assert!(buf.len() >= 2, "gzip stream too short ({} bytes)", buf.len());
    assert_eq!(
        buf[0], 0x1f,
        "first byte must be gzip magic 0x1f; got 0x{:02x}",
        buf[0]
    );
    assert_eq!(
        buf[1], 0x8b,
        "second byte must be gzip magic 0x8b; got 0x{:02x}",
        buf[1]
    );

    cleanup();
}

/// Decoding the gzipped stream must yield exactly the same bytes as
/// the uncompressed [`HeapProfile::write_pprof`] under the same
/// arguments.  This is the equivalence guarantee that lets the new
/// helper drop into any existing pprof-driven dashboard.
#[test]
fn write_pprof_gz_round_trips_to_write_pprof() {
    let _lock = workload_lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let (snap, cleanup) = run_workload(50);

    // Encode both forms with the same Weight to make the comparison
    // structurally meaningful.
    let weight = Weight::Allocated;

    let mut gz: Vec<u8> = Vec::new();
    snap.write_pprof_gz(&mut gz, weight)
        .expect("Vec<u8> write is infallible");

    let mut uncompressed: Vec<u8> = Vec::new();
    snap.write_pprof(&mut uncompressed, weight)
        .expect("Vec<u8> write is infallible");

    let mut decoded: Vec<u8> = Vec::new();
    flate2::read::GzDecoder::new(gz.as_slice())
        .read_to_end(&mut decoded)
        .expect("gzip decode succeeds");

    assert_eq!(
        decoded.len(),
        uncompressed.len(),
        "decoded gz payload length ({}) != write_pprof length ({})",
        decoded.len(),
        uncompressed.len()
    );
    assert_eq!(
        decoded, uncompressed,
        "decoded gzipped pprof must match the uncompressed pprof byte-for-byte"
    );

    // Sanity: gzip must not have expanded the payload to something
    // smaller than the gzip header itself.  RFC 1952 minimum header
    // is 10 bytes, plus the 8-byte trailer.  This is a guard against
    // accidentally emitting an empty stream (e.g. if `finish()` were
    // ever dropped).
    assert!(
        gz.len() >= 18,
        "gz output suspiciously short ({} bytes) -- missing header/trailer?",
        gz.len()
    );

    cleanup();
}

/// Empty snapshot -> valid gzip stream -> decoded payload equals
/// `write_pprof` on the same empty snapshot.  Runs in both feature
/// configs would require relaxing the file-level `cfg`, but the
/// profiling-OFF build already takes the same code path (every
/// snapshot is empty by construction), so this test fully covers it.
#[test]
fn write_pprof_gz_empty_snapshot() {
    let p = HeapProfile::default();
    assert!(p.is_empty());

    let mut gz: Vec<u8> = Vec::new();
    p.write_pprof_gz(&mut gz, Weight::Allocated)
        .expect("empty profile write is infallible");

    // Still a valid gzip stream.
    assert!(gz.len() >= 2);
    assert_eq!(gz[0], 0x1f);
    assert_eq!(gz[1], 0x8b);

    // Decoded payload equals uncompressed write_pprof on the same
    // empty snapshot -- which we've already validated in the
    // `write_pprof_empty_snapshot` test in the sibling file.
    let mut uncompressed: Vec<u8> = Vec::new();
    p.write_pprof(&mut uncompressed, Weight::Allocated)
        .expect("empty profile write is infallible");

    let mut decoded: Vec<u8> = Vec::new();
    flate2::read::GzDecoder::new(gz.as_slice())
        .read_to_end(&mut decoded)
        .expect("gzip decode succeeds even on tiny payload");

    assert_eq!(
        decoded, uncompressed,
        "decoded empty-snapshot pprof must match the uncompressed encoding"
    );
}
