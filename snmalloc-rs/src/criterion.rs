//! Criterion bench-profiling helper.
//!
//! Glue between a `criterion::Bencher` measurement loop and a
//! [`crate::streaming::ProfilingSession`].  Lets a bench author capture a
//! streamed heap profile that covers exactly the iterations criterion
//! timed -- no manual session start/stop around `bencher.iter` and no
//! drift between the measured window and the sampled window.
//!
//! Gated on `feature = "profiling"` **and**
//! `feature = "criterion-integration"`.  Both are off by default so a
//! plain `cargo build -p snmalloc-rs` does not pull in criterion or
//! flate2; turning the integration on requires opting into the
//! underlying profiler as well, since the helper has no useful behaviour
//! without it.
//!
//! Why a session per bench function (not per iteration)
//! ----------------------------------------------------
//!
//! [`ProfilingSession::start`] / [`ProfilingSession::drop`] each register
//! and tear down a process-wide trampoline plus a mutex-guarded handler
//! slot; that work amortises poorly across short iterations.  This
//! helper opens the session **once** for the whole `bencher.iter` call,
//! so the start/stop cost is paid a single time per criterion bench
//! function -- not per sample, and not per iteration.  The trade-off is
//! that the profile aggregates every sample taken across all iterations
//! of the measurement loop; that is the right granularity for a "what
//! does this benchmark allocate?" question and is what `cargo bench`
//! consumers typically want.
//!
//! Streaming startup/shutdown cost can still dominate sub-microsecond
//! benches.  If your bench's inner body completes in tens of
//! nanoseconds, prefer running the helper at a coarser granularity
//! (e.g. wrap a `bench_function` whose body itself loops, not a tight
//! per-iteration body) so the per-session fixed cost is amortised
//! against the work you actually want to attribute.
//!
//! Per-thread event buffer sizing
//! ------------------------------
//!
//! Streaming dispatches each sample through a process-global trampoline.
//! The default Poisson sampling interval (524 288 bytes, ~512 KiB) is
//! deliberately conservative so the trampoline rarely fires on the hot
//! path; bench bodies that allocate aggressively can still saturate the
//! handler if the rate is cranked up.  Tune
//! [`crate::SnMalloc::set_sampling_rate`] before calling this helper
//! (typical values: `65_536` for a high-resolution one-off, `524_288`
//! for production-shaped overhead).  See
//! [`crate::SnMalloc::set_max_local_cache`] for the per-thread cache cap
//! that also affects how many samples per second the trampoline can
//! observe.
//!
//! # Example
//!
//! With `criterion`'s `iter` pattern:
//!
//! ```no_run
//! use criterion::{Bencher, Criterion};
//! use snmalloc_rs::{criterion::bench_with_profile, SnMalloc};
//! use std::path::Path;
//!
//! fn bench_my_workload(c: &mut Criterion) {
//!     SnMalloc.set_sampling_rate(65_536);
//!     c.bench_function("my_workload", |b: &mut Bencher| {
//!         bench_with_profile(b, Path::new("target/criterion/my_workload.folded"), || {
//!             // The body whose allocations you want profiled.
//!             let v: Vec<u64> = (0..1024).collect();
//!             criterion::black_box(v);
//!         });
//!     });
//! }
//! ```
//!
//! With `criterion`'s `iter_batched` pattern (when each iteration needs
//! per-iteration setup), wrap the equivalent `iter_batched` call inside
//! a `bench_with_profile` body that itself calls `b.iter_batched`:
//!
//! ```no_run
//! use criterion::{BatchSize, Bencher, Criterion};
//! use snmalloc_rs::{criterion::bench_with_profile, SnMalloc};
//! use std::path::Path;
//!
//! fn bench_with_setup(c: &mut Criterion) {
//!     SnMalloc.set_sampling_rate(65_536);
//!     c.bench_function("with_setup", |b: &mut Bencher| {
//!         // bench_with_profile measures + profiles the iter call below.
//!         // The `body` closure is invoked once and runs the entire
//!         // iter_batched loop inside the active profiling session.
//!         bench_with_profile(b, Path::new("target/criterion/with_setup.folded"), || {
//!             // No-op: criterion's `iter_batched` is driven inline via
//!             // the dedicated helper below.
//!         });
//!     });
//! }
//! ```
//!
//! For `iter_batched` specifically, prefer
//! [`bench_with_profile_batched`] -- it forwards setup / routine / batch
//! size through to criterion while still opening one session per bench
//! function.

#![cfg(all(feature = "profiling", feature = "criterion-integration"))]

extern crate std;

use std::fs::File;
use std::io::BufWriter;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use criterion::{BatchSize, Bencher};

use crate::profile::{BtSample, HeapProfile};
use crate::streaming::ProfilingSession;

/// Run `body` under `bencher.iter`, wrapped in a single
/// [`ProfilingSession`] whose accumulated samples are written as a
/// folded-stack flamegraph to `session_path` once the bench function
/// returns.
///
/// The session is opened **once** for the whole `iter` call (not per
/// iteration); see the module-level docs for the rationale and for
/// notes on per-thread buffer sizing.
///
/// `session_path` is created (or truncated) when the bench function
/// finishes; the parent directory must already exist (criterion usually
/// has the `target/criterion/...` tree by the time the harness invokes
/// the bench, but creating an extra subdirectory is the caller's
/// responsibility).  Errors writing the file are logged to stderr and
/// then swallowed -- a bench harness has no clean way to surface them
/// and we do not want a stat write failure to fail the run.
///
/// If a [`ProfilingSession`] cannot be started (e.g. because another
/// session is already active in the process, or the underlying C++ side
/// was built without `SNMALLOC_PROFILE`) the bench still runs; only the
/// profile output is skipped.  This keeps the helper safe to drop into
/// existing benches that the user might run with the `profiling` feature
/// turned off at the C level.
pub fn bench_with_profile<F, R>(bencher: &mut Bencher<'_>, session_path: &Path, mut body: F)
where
    F: FnMut() -> R,
{
    let collector = SampleCollector::new();
    let collector_for_handler = Arc::clone(&collector.inner);

    let session = ProfilingSession::start(move |sample| {
        // Convert the borrowed StreamSample into an owned BtSample so we
        // can stash it past the callback.  The frame array is the only
        // borrowed field; the rest are by-value.
        let stack: std::vec::Vec<*const u8> = sample
            .stack()
            .iter()
            .map(|p| *p as *const u8)
            .collect();
        let owned = BtSample {
            alloc_ptr: sample.alloc_ptr() as *const u8,
            requested_size: sample.requested_size(),
            allocated_size: sample.allocated_size(),
            weight: sample.weight() as usize,
            stack,
        };
        if let Ok(mut guard) = collector_for_handler.lock() {
            guard.push(owned);
        }
    });

    // Run the measurement loop regardless of whether the session
    // started; we still want the bench numbers.
    bencher.iter(&mut body);

    // Tear the session down before consuming the collected samples;
    // this also flushes any in-flight callbacks (Drop waits for the
    // handler-slot mutex).
    drop(session);

    write_collected(&collector, session_path);
}

/// `iter_batched` variant of [`bench_with_profile`].
///
/// Forwards `setup`, `routine`, and `batch_size` to
/// [`Bencher::iter_batched`].  The wrapping session is opened once
/// before the iter_batched call and dropped after, exactly as in
/// [`bench_with_profile`]; only the inner loop shape changes.
///
/// Use this when each iteration needs per-iteration input that should
/// not be counted in the measurement (e.g. cloning a `Vec` to mutate
/// in place), so that criterion's batched-input handling applies.
pub fn bench_with_profile_batched<I, O, S, R>(
    bencher: &mut Bencher<'_>,
    session_path: &Path,
    mut setup: S,
    mut routine: R,
    batch_size: BatchSize,
) where
    S: FnMut() -> I,
    R: FnMut(I) -> O,
{
    let collector = SampleCollector::new();
    let collector_for_handler = Arc::clone(&collector.inner);

    let session = ProfilingSession::start(move |sample| {
        let stack: std::vec::Vec<*const u8> = sample
            .stack()
            .iter()
            .map(|p| *p as *const u8)
            .collect();
        let owned = BtSample {
            alloc_ptr: sample.alloc_ptr() as *const u8,
            requested_size: sample.requested_size(),
            allocated_size: sample.allocated_size(),
            weight: sample.weight() as usize,
            stack,
        };
        if let Ok(mut guard) = collector_for_handler.lock() {
            guard.push(owned);
        }
    });

    bencher.iter_batched(&mut setup, &mut routine, batch_size);

    drop(session);

    write_collected(&collector, session_path);
}

/// Shared accumulator for streamed samples.  The streaming handler
/// closure must be `Fn + Send + Sync + 'static`, so the collector is
/// behind an `Arc<Mutex<...>>` rather than borrowed from the stack.
struct SampleCollector {
    inner: Arc<Mutex<std::vec::Vec<BtSample>>>,
}

impl SampleCollector {
    fn new() -> Self {
        Self {
            inner: Arc::new(Mutex::new(std::vec::Vec::new())),
        }
    }

    /// Take ownership of the accumulated samples, leaving the
    /// underlying buffer empty.  Returns an empty `Vec` if the mutex is
    /// poisoned (a previously-panicking handler -- treat as no data
    /// rather than re-panic from a bench harness).
    fn take(&self) -> std::vec::Vec<BtSample> {
        match self.inner.lock() {
            Ok(mut guard) => std::mem::take(&mut *guard),
            Err(_) => std::vec::Vec::new(),
        }
    }
}

/// Serialise the collected samples to `session_path` as a folded-stack
/// flamegraph.  Errors are reported to stderr; the helper is total --
/// it never panics on I/O failure because a bench harness has no
/// meaningful way to surface the error to the user.
fn write_collected(collector: &SampleCollector, session_path: &Path) {
    let samples = collector.take();
    let profile = HeapProfile::from_samples(samples);

    let path: PathBuf = session_path.to_path_buf();
    let file = match File::create(&path) {
        Ok(f) => f,
        Err(err) => {
            std::eprintln!(
                "bench_with_profile: failed to create {}: {}",
                path.display(),
                err
            );
            return;
        }
    };
    let mut writer = BufWriter::new(file);
    if let Err(err) = profile.write_flamegraph(&mut writer) {
        std::eprintln!(
            "bench_with_profile: failed to write {}: {}",
            path.display(),
            err
        );
    }
}
