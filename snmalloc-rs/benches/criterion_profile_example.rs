//! Example bench demonstrating [`snmalloc_rs::criterion::bench_with_profile`].
//!
//! Ticket 86aj2dww6 -- shows the recommended wiring for capturing a
//! folded-stack heap profile that covers exactly the iterations
//! criterion timed.  Two patterns are covered:
//!
//! 1. The plain `bencher.iter` path, via [`bench_with_profile`].
//! 2. The `bencher.iter_batched` path (per-iteration input setup), via
//!    [`bench_with_profile_batched`].
//!
//! Build with:
//!
//! ```text
//! cargo build -p snmalloc-rs --features profiling,criterion-integration \
//!     --benches
//! ```
//!
//! Run (after building) with:
//!
//! ```text
//! cargo bench -p snmalloc-rs --features profiling,criterion-integration \
//!     --bench criterion_profile_example
//! ```
//!
//! After the run completes, the folded-stack profiles land at
//! `target/criterion/<bench>.folded`.  Render with `inferno-flamegraph`
//! (or feed to speedscope, Pyroscope, etc.):
//!
//! ```text
//! inferno-flamegraph < target/criterion/example_iter.folded \
//!     > target/criterion/example_iter.svg
//! ```
//!
//! The bench is `#[cfg]`-gated on both `profiling` and
//! `criterion-integration` so that:
//!
//! - `cargo build -p snmalloc-rs` (default) still works -- the bench
//!   file compiles into an empty `main` and links cleanly without
//!   pulling criterion in.
//! - `cargo build -p snmalloc-rs --features profiling,criterion-integration`
//!   compiles the full bench against `snmalloc_rs::criterion`.

#![cfg_attr(
    not(all(feature = "profiling", feature = "criterion-integration")),
    allow(unused_imports, dead_code)
)]

#[cfg(all(feature = "profiling", feature = "criterion-integration"))]
mod inner {
    use std::path::Path;
    use std::time::Duration;

    use criterion::{black_box, BatchSize, Criterion};

    use snmalloc_rs::criterion::{bench_with_profile, bench_with_profile_batched};
    use snmalloc_rs::SnMalloc;

    /// Plain `iter` example: allocate a vector inside the bench body,
    /// then black_box it to keep the optimiser honest.  The folded
    /// profile lands at `target/criterion/example_iter.folded`.
    pub fn example_iter(c: &mut Criterion) {
        // Crank the sampling rate up so a short bench still captures
        // some samples; production profiling typically wants 256 KiB
        // or larger.  64 KiB strikes a usable balance here.
        SnMalloc.set_sampling_rate(65_536);

        c.bench_function("example_iter", |b| {
            bench_with_profile(
                b,
                Path::new("target/criterion/example_iter.folded"),
                || {
                    // Body that does some allocation.  In a real bench
                    // this would call into the code under test.
                    let v: Vec<u64> = (0..1024).collect();
                    black_box(v);
                },
            );
        });
    }

    /// `iter_batched` example: per-iteration setup builds an input
    /// `Vec` outside the timed window; the routine sorts it in place.
    /// The folded profile lands at
    /// `target/criterion/example_iter_batched.folded`.
    pub fn example_iter_batched(c: &mut Criterion) {
        SnMalloc.set_sampling_rate(65_536);

        c.bench_function("example_iter_batched", |b| {
            bench_with_profile_batched(
                b,
                Path::new("target/criterion/example_iter_batched.folded"),
                || {
                    // Setup: build the input.  Not measured.
                    (0..1024u64).rev().collect::<Vec<u64>>()
                },
                |mut v| {
                    // Routine: the timed-and-profiled work.
                    v.sort();
                    black_box(v);
                },
                BatchSize::SmallInput,
            );
        });
    }

    pub fn configure() -> Criterion {
        Criterion::default()
            .warm_up_time(Duration::from_secs(1))
            .measurement_time(Duration::from_secs(2))
            .sample_size(20)
    }
}

#[cfg(all(feature = "profiling", feature = "criterion-integration"))]
criterion::criterion_group! {
    name = profile_helper_benches;
    config = inner::configure();
    targets = inner::example_iter, inner::example_iter_batched
}

#[cfg(all(feature = "profiling", feature = "criterion-integration"))]
criterion::criterion_main!(profile_helper_benches);

// Feature-off build: keep the bench binary compilable so
// `cargo build -p snmalloc-rs --benches` (no feature flags) still
// succeeds.  The empty `main` is a no-op and the binary will simply
// exit 0 if executed.
#[cfg(not(all(feature = "profiling", feature = "criterion-integration")))]
fn main() {}
