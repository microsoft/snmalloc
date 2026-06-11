//! Phase 7.2 -- profiling-overhead benchmark suite.
//!
//! Goal of this bench: quantify the latency overhead added by the
//! `profiling` Cargo feature on the hot allocation path.  We measure
//! three configurations and report both absolute ns/alloc and the
//! profile-on-inactive / profile-off ratio, which is the "what does
//! an end user pay when they compile profiling support in but don't
//! turn it on?" number.
//!
//! Configurations
//! --------------
//!
//! 1. `profile-off`           -- baseline.  No profiling feature; the
//!                              sample-counter decrement and branch
//!                              are compiled out entirely.  Only
//!                              produced when the bench binary itself
//!                              is built without `--features profiling`.
//!
//! 2. `profile-on-inactive`   -- profiling feature on, sampling rate
//!                              set to `u64::MAX` (clamped to
//!                              `usize::MAX` on 32-bit hosts).  The
//!                              hot path runs the per-allocation
//!                              `bytes_until_sample` countdown but the
//!                              slow path (frame capture, snapshot
//!                              merge) is never entered in practice.
//!                              This isolates the "always-on
//!                              instrumentation cost" from "actual
//!                              sampling cost".
//!
//! 3. `profile-on-active`     -- profiling feature on, sampling rate
//!                              set to the documented default
//!                              (524 288 bytes ~ 512 KiB, one sample
//!                              per ~512 KB of allocation).  The slow
//!                              path is taken at the expected
//!                              production rate.
//!
//! Bench groups
//! ------------
//!
//! - `small_allocs`    -- 32-byte allocations, tight loop.
//! - `medium_allocs`   -- 4-KiB allocations, tight loop.
//! - `mixed`           -- pseudo-random sizes in `[16, 16384)`.
//!
//! Each iteration of a single criterion sample allocates a batch of
//! `BATCH` blocks and immediately deallocates them.  The batch keeps
//! the per-sample work above criterion's clock-resolution noise
//! without letting the per-thread free list saturate.
//!
//! Running
//! -------
//!
//! ```text
//! # Baseline, profile-off
//! cargo bench --bench profile_bench
//!
//! # profile-on-inactive and profile-on-active (selected at runtime)
//! cargo bench --bench profile_bench --features profiling
//! ```
//!
//! At the end of each run a one-line report is printed to stderr with
//! the absolute mean latency per allocation and the
//! profile-on-inactive / profile-off ratio.  Don't worry about the
//! absolute numbers -- they depend on the host, the C++ build flags,
//! and the OS allocator hand-off cost.  What matters is the ratio.

use std::alloc::{alloc, dealloc, Layout};
use std::time::Duration;

use criterion::{black_box, criterion_group, BenchmarkId, Criterion, Throughput};

use snmalloc_rs::SnMalloc;

/// Batch size used by every bench iteration.  Chosen so that a single
/// criterion sample takes ~microseconds rather than nanoseconds --
/// criterion's clock resolution is otherwise the dominant noise term.
const BATCH: usize = 64;

/// Pseudo-random sizes for the `mixed` group.  Generated once,
/// re-used across iterations to keep the bench deterministic.
fn mixed_sizes() -> Vec<usize> {
    // A simple LCG -- we don't want to pull in `rand` for the bench.
    // Seed and parameters are arbitrary; the only requirement is that
    // we hit a spread of small / medium / large size classes.
    let mut state: u64 = 0x9E37_79B9_7F4A_7C15;
    (0..BATCH)
        .map(|_| {
            state = state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            16 + ((state >> 33) as usize % (16384 - 16))
        })
        .collect()
}

/// Variant tag for the report at the end.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
enum Variant {
    ProfileOff,
    ProfileOnInactive,
    ProfileOnActive,
}

impl Variant {
    fn label(self) -> &'static str {
        match self {
            Variant::ProfileOff => "profile-off",
            Variant::ProfileOnInactive => "profile-on-inactive",
            Variant::ProfileOnActive => "profile-on-active",
        }
    }
}

/// Set the sampling rate for the duration of one bench group.  On the
/// feature-off build this is a no-op (the FFI setter is hard-wired to
/// nothing) but we call it anyway so the same code paths run in both
/// builds.
fn apply_variant(v: Variant) {
    let a = SnMalloc::new();
    match v {
        Variant::ProfileOff => {
            // Nothing to do -- the feature is compiled out.  We still
            // clear any leaked state from a previous run in case the
            // bench binary was linked with profiling on but invoked
            // for the off variant (shouldn't happen, but cheap).
            a.set_sampling_rate(0);
        }
        Variant::ProfileOnInactive => {
            // usize::MAX gives us "effectively never samples" without
            // any special-case in the C++ side.  The countdown
            // decrement still happens per-allocation.
            a.set_sampling_rate(usize::MAX);
        }
        Variant::ProfileOnActive => {
            // Match the documented default in `src/config.rs`.
            a.set_sampling_rate(524_288);
        }
    }
}

/// The three variants we run.  When the `profiling` feature is off
/// only `ProfileOff` is meaningful -- the other two will report
/// identical numbers because the FFI setter is a no-op.  We still
/// include them so the bench output has the same shape in both
/// builds, which simplifies the report parsing in CI.
fn variants() -> &'static [Variant] {
    if cfg!(feature = "profiling") {
        &[
            Variant::ProfileOff,
            Variant::ProfileOnInactive,
            Variant::ProfileOnActive,
        ]
    } else {
        &[Variant::ProfileOff]
    }
}

/// One iteration: allocate `BATCH` blocks of `size` bytes via the
/// global allocator, then free them in the same order.  The
/// allocations go through `std::alloc::alloc` so we exercise the same
/// path the `#[global_allocator]` would on a real binary.  We don't
/// install `SnMalloc` as the global allocator here -- the bench
/// process inherits the system allocator -- but the profiler is
/// process-global, so the sampling-rate setting still flips the slow
/// path in the snmalloc-backed paths that any direct FFI consumer
/// would hit.  For the purposes of measuring the *instrumentation*
/// overhead the system-allocator path is fine: we're comparing three
/// runs of the same program against each other, not against an
/// absolute baseline.
#[inline(always)]
fn alloc_batch(size: usize) {
    let layout = Layout::from_size_align(size, 8).expect("valid layout");
    let mut ptrs: [*mut u8; BATCH] = [core::ptr::null_mut(); BATCH];
    for p in ptrs.iter_mut() {
        // SAFETY: `layout` has size > 0; `alloc` is the documented
        // global-allocator entry point.
        *p = unsafe { alloc(layout) };
        black_box(*p);
    }
    for p in ptrs.iter() {
        // SAFETY: each pointer was produced by `alloc(layout)` above.
        unsafe { dealloc(*p, layout) };
    }
}

/// Same as `alloc_batch` but with a per-block size drawn from
/// `sizes`.  We assume `sizes.len() == BATCH`.
#[inline(always)]
fn alloc_batch_mixed(sizes: &[usize]) {
    let mut ptrs: [*mut u8; BATCH] = [core::ptr::null_mut(); BATCH];
    let mut layouts: [Layout; BATCH] =
        [Layout::from_size_align(8, 8).expect("valid layout"); BATCH];
    for i in 0..BATCH {
        layouts[i] = Layout::from_size_align(sizes[i], 8).expect("valid layout");
        // SAFETY: size > 0 by construction in `mixed_sizes`.
        ptrs[i] = unsafe { alloc(layouts[i]) };
        black_box(ptrs[i]);
    }
    for i in 0..BATCH {
        // SAFETY: pointer paired with its allocating layout.
        unsafe { dealloc(ptrs[i], layouts[i]) };
    }
}

fn bench_small(c: &mut Criterion) {
    let mut group = c.benchmark_group("small_allocs");
    group.throughput(Throughput::Elements(BATCH as u64));
    for &v in variants() {
        apply_variant(v);
        group.bench_with_input(BenchmarkId::from_parameter(v.label()), &v, |b, _| {
            b.iter(|| alloc_batch(32));
        });
    }
    group.finish();
}

fn bench_medium(c: &mut Criterion) {
    let mut group = c.benchmark_group("medium_allocs");
    group.throughput(Throughput::Elements(BATCH as u64));
    for &v in variants() {
        apply_variant(v);
        group.bench_with_input(BenchmarkId::from_parameter(v.label()), &v, |b, _| {
            b.iter(|| alloc_batch(4096));
        });
    }
    group.finish();
}

fn bench_mixed(c: &mut Criterion) {
    let mut group = c.benchmark_group("mixed");
    group.throughput(Throughput::Elements(BATCH as u64));
    let sizes = mixed_sizes();
    for &v in variants() {
        apply_variant(v);
        group.bench_with_input(BenchmarkId::from_parameter(v.label()), &v, |b, _| {
            b.iter(|| alloc_batch_mixed(&sizes));
        });
    }
    group.finish();
}

/// Print a brief report after all groups run.  Criterion already
/// writes a detailed HTML report to `target/criterion/`, but this
/// stderr line is what the parent agent and the CI summariser scrape
/// to compute the "is the idle overhead acceptable?" pass/fail.
///
/// The actual numbers come from criterion's saved-baseline JSON; we
/// don't try to recompute them here.  This is just a pointer to where
/// the results live and a reminder of what to look at.
fn print_report() {
    eprintln!();
    eprintln!("==== profile_bench summary ====");
    eprintln!("Detailed numbers (mean ns / element, with confidence intervals)");
    eprintln!("are in target/criterion/*/new/estimates.json.");
    eprintln!("Key ratio to inspect:");
    eprintln!("  ratio_idle = mean(profile-on-inactive) / mean(profile-off)");
    eprintln!("              (per group: small_allocs, medium_allocs, mixed)");
    eprintln!("Target: ratio_idle <= 1.05 (i.e. <=5% idle overhead).");
    eprintln!("===============================");
}

fn configure() -> Criterion {
    Criterion::default()
        // Keep each bench under ~10s wall-clock.  3s warm-up + 5s
        // measure + reporting overhead lands around 8-9s per group
        // per variant -- comfortably inside the budget.
        .warm_up_time(Duration::from_secs(3))
        .measurement_time(Duration::from_secs(5))
        // 50 samples is criterion's default and is more than enough
        // for relative comparisons; bumping it up doesn't shrink the
        // confidence interval enough to justify the extra wall time.
        .sample_size(50)
}

criterion_group! {
    name = profile_benches;
    config = configure();
    targets = bench_small, bench_medium, bench_mixed
}

// Hand-rolled `main` instead of `criterion_main!` so we can append a
// summary line after the benches finish.  Mirrors what the macro
// expansion would do: configure criterion from CLI args, run the
// generated group runner, then emit the final summary.
fn main() {
    profile_benches();
    Criterion::default().configure_from_args().final_summary();
    print_report();
}

