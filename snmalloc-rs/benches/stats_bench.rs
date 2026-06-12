//! Phase 11.1 -- SNMALLOC_STATS=ON acceptance bench.
//!
//! Goal of this bench: quantify the latency overhead added by the
//! `stats` Cargo feature on the hot allocation path.  Spec target is
//! `ratio_stats_on / ratio_stats_off <= 1.02` on the existing
//! criterion groups (`small_allocs`, `medium_allocs`, `mixed`).
//!
//! Unlike `profile_bench.rs` (which routes through `std::alloc` and
//! therefore lands on the host's libc allocator -- see the
//! "Verification follow-up" subsection in `docs/heap-profiling-
//! benchmarks.md`), this bench installs `SnMalloc` as the
//! `#[global_allocator]` so each iteration actually exercises the
//! `sn_rust_alloc` / `sn_rust_dealloc` FFI thunks, which is where
//! the SNMALLOC_STATS counter sites live.  Without that the bench
//! would measure libc and produce a ratio of ~1.0 regardless of
//! whether the stats feature was on.
//!
//! Variants
//! --------
//!
//! Cargo features are *compile-time* gates -- a single bench binary
//! cannot toggle SNMALLOC_STATS at runtime.  The off/on comparison
//! is therefore done across two invocations of `cargo bench`:
//!
//! ```text
//! # Baseline -- SNMALLOC_STATS compiled out
//! cargo bench --bench stats_bench
//!
//! # Stats on -- SNMALLOC_STATS=ON in the C++ build
//! cargo bench --features stats --bench stats_bench
//! ```
//!
//! The criterion baseline machinery (`--save-baseline` /
//! `--baseline`) is the recommended way to compare the two runs;
//! see `docs/heap-profiling-benchmarks.md` ("Phase 9 stats
//! overhead") for the exact procedure used to produce the
//! published 5-run mean.
//!
//! Bench groups
//! ------------
//!
//! - `small_allocs`    -- 32-byte allocations, tight loop.
//! - `medium_allocs`   -- 4-KiB allocations, tight loop.
//! - `mixed`           -- LCG-driven sizes in `[16, 16384)`.
//!
//! Each iteration of a single criterion sample allocates a batch of
//! `BATCH` blocks via the global allocator and immediately frees
//! them in the same order.  Batch size, warm-up, measure-time, and
//! sample-count mirror `profile_bench.rs` so the two suites can be
//! compared cell-for-cell.

use std::alloc::{alloc, dealloc, Layout};
use std::time::Duration;

use criterion::{black_box, criterion_group, BenchmarkId, Criterion, Throughput};

use snmalloc_rs::SnMalloc;

/// Install snmalloc as the process-wide allocator so the bench's
/// `std::alloc::{alloc, dealloc}` calls land in the
/// `sn_rust_alloc` / `sn_rust_dealloc` FFI thunks where the
/// SNMALLOC_STATS counter sites live.  Without this the bench
/// would measure libc malloc and the stats feature would have no
/// observable effect.
#[global_allocator]
static GLOBAL: SnMalloc = SnMalloc;

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

/// Tag used in the criterion group label.  We compile a single
/// variant per binary -- the feature flag picks which one -- and use
/// `cfg!(feature = "stats")` to label the criterion sample so the
/// two `cargo bench` runs land in distinct `target/criterion`
/// sub-directories.
fn variant_label() -> &'static str {
    if cfg!(feature = "stats") {
        "stats-on"
    } else {
        "stats-off"
    }
}

/// One iteration: allocate `BATCH` blocks of `size` bytes via the
/// global allocator (snmalloc, installed via `#[global_allocator]`
/// above) and free them in the same order.  Each call lands in
/// `sn_rust_alloc` / `sn_rust_dealloc` -- the FFI thunks that carry
/// the SNMALLOC_STATS counter sites -- so the bench is sensitive to
/// the stats feature in a way `profile_bench.rs` (which intentionally
/// stays on libc) is not.
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
    group.bench_with_input(
        BenchmarkId::from_parameter(variant_label()),
        &(),
        |b, _| {
            b.iter(|| alloc_batch(32));
        },
    );
    group.finish();
}

fn bench_medium(c: &mut Criterion) {
    let mut group = c.benchmark_group("medium_allocs");
    group.throughput(Throughput::Elements(BATCH as u64));
    group.bench_with_input(
        BenchmarkId::from_parameter(variant_label()),
        &(),
        |b, _| {
            b.iter(|| alloc_batch(4096));
        },
    );
    group.finish();
}

fn bench_mixed(c: &mut Criterion) {
    let mut group = c.benchmark_group("mixed");
    group.throughput(Throughput::Elements(BATCH as u64));
    let sizes = mixed_sizes();
    group.bench_with_input(
        BenchmarkId::from_parameter(variant_label()),
        &(),
        |b, _| {
            b.iter(|| alloc_batch_mixed(&sizes));
        },
    );
    group.finish();
}

/// Print a brief report after all groups run.  The full per-group
/// numbers come from criterion's saved JSON; this stderr line is
/// what the parent agent and the CI summariser scrape to find the
/// pointer to the raw data.
fn print_report() {
    eprintln!();
    eprintln!("==== stats_bench summary ({}) ====", variant_label());
    eprintln!("Detailed numbers (mean ns / element, with confidence intervals)");
    eprintln!("are in target/criterion/*/{}/new/estimates.json.", variant_label());
    eprintln!("Key ratio to inspect across two runs of this bench:");
    eprintln!("  ratio_stats = mean(stats-on) / mean(stats-off)");
    eprintln!("              (per group: small_allocs, medium_allocs, mixed)");
    eprintln!("Acceptance target: ratio_stats <= 1.02 (i.e. <=2% overhead).");
    eprintln!("===============================");
}

fn configure() -> Criterion {
    Criterion::default()
        // Keep each bench under ~10s wall-clock.  3s warm-up + 5s
        // measure + reporting overhead lands around 8-9s per group --
        // comfortably inside the budget.  Matches profile_bench.rs so
        // the two suites are directly comparable.
        .warm_up_time(Duration::from_secs(3))
        .measurement_time(Duration::from_secs(5))
        // 50 samples is criterion's default and is more than enough
        // for relative comparisons; bumping it up doesn't shrink the
        // confidence interval enough to justify the extra wall time.
        .sample_size(50)
}

criterion_group! {
    name = stats_benches;
    config = configure();
    targets = bench_small, bench_medium, bench_mixed
}

// Hand-rolled `main` instead of `criterion_main!` so we can append a
// summary line after the benches finish.  Mirrors what the macro
// expansion would do: configure criterion from CLI args, run the
// generated group runner, then emit the final summary.
fn main() {
    stats_benches();
    Criterion::default().configure_from_args().final_summary();
    print_report();
}
