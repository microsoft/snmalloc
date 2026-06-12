//! Phase 6.2 -- external-viewer round-trip for the pprof Profile
//! emitted by [`HeapProfile::write_pprof`].
//!
//! Phase 6.1 (PR #18) already covers structural validation: we feed
//! the encoded bytes through a 60-line in-test decoder and check
//! field shapes, axis names, and weight totals.  That tells us our
//! encoder is internally consistent.  What it does *not* tell us is
//! whether a third-party pprof consumer -- specifically the canonical
//! one, Google's `go tool pprof` -- will actually accept the file.
//!
//! This test runs `go tool pprof -raw <file>` as a subprocess and
//! requires:
//!
//! 1.  The subprocess exits with status zero (the file parsed).
//! 2.  stdout contains at least one of the structural markers
//!     `go tool pprof -raw` prints for a well-formed Profile
//!     (`Samples:` header, or the axis-name strings `alloc_space` /
//!     `alloc_objects` from our sample_type table).
//!
//! Graceful skip
//! -------------
//!
//! `go` is not part of the snmalloc CI image and we don't want this
//! test to flip CI red on a Rust-only developer's laptop.  The
//! [`skip_if_no_go`] helper at the top of the file probes for the
//! `go` binary up front; if it isn't on `PATH` we print a one-line
//! `eprintln!` ("test skipped: `go` not on PATH") and return without
//! failing.  CI configurations that *do* want to enforce this round
//! trip -- the long-term plan is a dedicated job in the heap-
//! profiling milestone -- will install Go and inherit the assertion
//! path automatically.
//!
//! Temp file convention
//! --------------------
//!
//! Per the Phase 6.2 spec, no new dev-deps.  We don't pull in
//! `tempfile`; instead we synthesise a unique path under
//! [`std::env::temp_dir`] from `SystemTime::UNIX_EPOCH` nanos plus
//! [`std::process::id`] (to be safe against parallel test binaries
//! tripping on the same nanosecond, vanishingly rare but cheap to
//! guard against).  The file is removed on the success path; on a
//! failed assertion the panic propagates and `cargo test` reports
//! the location, with the leftover file in `/tmp` available for
//! manual inspection -- which is generally what you want when a
//! pprof round-trip fails.

#![cfg(feature = "profiling")]

use snmalloc_rs::{HeapProfile, SnMalloc, Weight};
use std::alloc::{GlobalAlloc, Layout};
use std::fs;
use std::io::Write;
use std::path::PathBuf;
use std::process::Command;
use std::sync::{Mutex, MutexGuard, OnceLock};
use std::time::SystemTime;

// =========================================================================
// `go` availability probe
// =========================================================================

/// Returns `true` if the `go` toolchain is *not* available on `PATH`
/// (i.e. the caller should skip the test).  We run `go version`
/// rather than just `command -v go` because some hermetic CI images
/// ship a `go` shim that fails on first invocation; we want the
/// skip path to cover those too.  Any I/O error or non-zero exit
/// counts as "not available".
fn skip_if_no_go() -> bool {
    let probe = Command::new("go").arg("version").output();
    match probe {
        Ok(out) if out.status.success() => false,
        Ok(out) => {
            eprintln!(
                "test skipped: `go version` exited {:?} (stderr: {:?})",
                out.status.code(),
                String::from_utf8_lossy(&out.stderr)
            );
            true
        }
        Err(e) => {
            eprintln!("test skipped: `go` not on PATH ({})", e);
            true
        }
    }
}

// =========================================================================
// Workload helpers -- mirror tests/profile_pprof.rs and
// tests/profile_viewer_roundtrip.rs.
// =========================================================================

const RATE: usize = 512;
const N_ALLOCS: usize = 5_000;
const SIZE: usize = 64;

/// Process-wide mutex so this binary doesn't race with sibling
/// workload-driving tests that mutate the global sampler.  Each
/// integration test compiles to its own binary, so this lock is
/// only shared between tests in *this* file.
fn workload_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

/// Drive a small workload, take a snapshot, and return it along with
/// a cleanup closure that frees the allocations and restores the
/// previous sampling rate.  Panics if fewer than `min_samples` were
/// captured -- that would mean the rest of the test is asserting on
/// a misleadingly empty file.
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
// Temp-file helper
// =========================================================================

/// Build a unique path under `std::env::temp_dir()` for our pprof
/// output.  We avoid pulling in the `tempfile` crate per the Phase
/// 6.2 spec.  The filename combines:
///
/// - the test name (so an accidental leftover is identifiable),
/// - `std::process::id()` (to disambiguate parallel test binaries),
/// - `SystemTime` nanos since the Unix epoch (to disambiguate
///   sequential invocations within the same process).
///
/// Nano-second collision between two `unique_pprof_path` calls in
/// the same process is theoretically possible on platforms with a
/// coarse clock, but in practice the two tests in this file run
/// serially under `workload_lock` and any nanosecond-level race is
/// dominated by the surrounding `Command::new("go")` cost.
fn unique_pprof_path(label: &str) -> PathBuf {
    let nanos = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let mut p = std::env::temp_dir();
    p.push(format!(
        "snmalloc-pprof-roundtrip-{}-{}-{}.pb",
        label,
        std::process::id(),
        nanos
    ));
    p
}

/// Markers any of which, if present in `go tool pprof -raw` stdout,
/// confirm the subprocess actually parsed and walked a Profile.
/// `Samples:` is the section header in modern `pprof` output.
/// `sample_type` and `PeriodType` cover older builds where the
/// dump prints the metadata block before any sample section.
/// The string-table entries `alloc_space` / `alloc_objects` are the
/// axis labels our encoder writes and they survive into `-raw`
/// output verbatim, so they make a good fallback marker when no
/// samples were emitted (the empty-snapshot case).
const PPROF_RAW_MARKERS: &[&str] = &[
    "Samples:",
    "sample_type",
    "PeriodType",
    "alloc_space",
    "alloc_objects",
];

/// Returns true if `haystack` contains any of the markers above.
fn has_pprof_marker(haystack: &str) -> bool {
    PPROF_RAW_MARKERS.iter().any(|m| haystack.contains(m))
}

// =========================================================================
// Tests
// =========================================================================

/// Live workload + write_pprof + `go tool pprof -raw` round trip.
/// Skipped (eprintln + early return, *not* a failure) when `go` is
/// not on PATH.
#[test]
fn pprof_roundtrip_via_go_tool() {
    let _lock = workload_lock();

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        // Same belt-and-braces pattern as the sibling tests: the
        // cfg gate at the top of the file already prevents this
        // binary from compiling without `profiling`, but if someone
        // turns the feature on against an OFF C++ build we still
        // want a clean skip.
        return;
    }

    if skip_if_no_go() {
        return;
    }

    let (snap, cleanup) = run_workload(50);

    // Encode to bytes.
    let mut buf: Vec<u8> = Vec::new();
    snap.write_pprof(&mut buf, Weight::Allocated)
        .expect("Vec<u8> write is infallible");
    assert!(!buf.is_empty(), "pprof bytes unexpectedly empty");

    // Persist to a tempfile.
    let path = unique_pprof_path("workload");
    {
        let mut f = fs::File::create(&path)
            .unwrap_or_else(|e| panic!("create {} failed: {}", path.display(), e));
        f.write_all(&buf)
            .unwrap_or_else(|e| panic!("write {} failed: {}", path.display(), e));
        // Drop closes the file before we hand it to the subprocess.
    }

    // Run `go tool pprof -raw <file>`.  We capture stdout + stderr
    // so a failure path can attribute the cause precisely.
    let out = Command::new("go")
        .args(["tool", "pprof", "-raw"])
        .arg(&path)
        .output()
        .unwrap_or_else(|e| panic!("spawning `go tool pprof` failed: {}", e));

    // Clean up the file before the assertion path: if the assertion
    // fires the panic message has the captured stdout/stderr; we
    // don't need the file lingering in /tmp on success.  On panic
    // we accept the (small) leak.
    let stdout = String::from_utf8_lossy(&out.stdout).to_string();
    let stderr = String::from_utf8_lossy(&out.stderr).to_string();
    let _ = fs::remove_file(&path);

    assert!(
        out.status.success(),
        "`go tool pprof -raw` exited {:?}\nstdout:\n{}\nstderr:\n{}",
        out.status.code(),
        stdout,
        stderr
    );
    assert!(
        has_pprof_marker(&stdout),
        "`go tool pprof -raw` stdout missing any structural marker \
         ({:?}); stdout was:\n{}\nstderr was:\n{}",
        PPROF_RAW_MARKERS,
        stdout,
        stderr
    );

    cleanup();
}

/// Empty profile + `go tool pprof -raw` round trip.  Zero samples is
/// a perfectly valid pprof Profile (our encoder still emits the two
/// sample_type axes and the `default_sample_type` hint), and
/// `go tool pprof` must accept it without error.  This is the path
/// the OFF C++ build would take if it were exposed to this binary --
/// every snapshot is empty under that configuration.
#[test]
fn empty_snapshot_pprof_roundtrip() {
    if skip_if_no_go() {
        return;
    }

    let p = HeapProfile::default();
    assert!(p.is_empty());

    let mut buf: Vec<u8> = Vec::new();
    p.write_pprof(&mut buf, Weight::Allocated)
        .expect("empty profile write is infallible");
    assert!(
        !buf.is_empty(),
        "even an empty Profile must contain sample_type axes + string \
         table; got zero bytes"
    );

    let path = unique_pprof_path("empty");
    {
        let mut f = fs::File::create(&path)
            .unwrap_or_else(|e| panic!("create {} failed: {}", path.display(), e));
        f.write_all(&buf)
            .unwrap_or_else(|e| panic!("write {} failed: {}", path.display(), e));
    }

    let out = Command::new("go")
        .args(["tool", "pprof", "-raw"])
        .arg(&path)
        .output()
        .unwrap_or_else(|e| panic!("spawning `go tool pprof` failed: {}", e));

    let stdout = String::from_utf8_lossy(&out.stdout).to_string();
    let stderr = String::from_utf8_lossy(&out.stderr).to_string();
    let _ = fs::remove_file(&path);

    assert!(
        out.status.success(),
        "`go tool pprof -raw` rejected an empty Profile; exited {:?}\n\
         stdout:\n{}\nstderr:\n{}",
        out.status.code(),
        stdout,
        stderr
    );
    // For an empty Profile there are no sample lines, but the
    // metadata section (sample_type / PeriodType / axis-name strings
    // from the string table) must still be present.  We don't insist
    // on `Samples:` here because some `pprof` builds elide the
    // section header when there are zero entries.
    assert!(
        has_pprof_marker(&stdout),
        "`go tool pprof -raw` stdout on empty Profile missing any \
         structural marker ({:?}); stdout was:\n{}\nstderr was:\n{}",
        PPROF_RAW_MARKERS,
        stdout,
        stderr
    );
}
