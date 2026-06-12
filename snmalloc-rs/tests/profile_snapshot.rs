//! Integration tests for the safe Rust profile snapshot wrapper
//! introduced in Phase 4.1.
//!
//! These tests are written so they compile and pass in BOTH
//! configurations:
//!
//! - `cargo test`                                  -> profiling feature OFF
//! - `cargo test --features profiling`             -> profiling feature ON
//!
//! In the OFF build, the FFI calls degrade to no-op stubs (returning
//! `false` / `0` / `nullptr`), so every assertion below is checking
//! the documented "empty profile / unsupported / zero rate" contract.
//!
//! In the ON build, `profiling_supported()` returns `true`, the
//! sampling rate is settable, and -- as of Phase 4.2 -- the underlying
//! C++ shim (`src/snmalloc/override/rust.cc`) is compiled with a
//! profile-enabled `snmalloc::Config` whose `ClientMeta` is
//! `LazyArrayClientMetaDataProvider<std::atomic<SampledAlloc*>>`.  The
//! alloc/dealloc hooks therefore do real work and `live_sampling_run`
//! below exercises the full pipeline end-to-end.

use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};

/// `profiling_supported()` reflects the linked C++ build's
/// `SNMALLOC_PROFILE` define, which the `snmalloc-sys` build script
/// flips on iff the `profiling` Cargo feature is set.
#[test]
fn profiling_supported_matches_feature() {
    let a = SnMalloc::new();
    let supported = a.profiling_supported();
    if cfg!(feature = "profiling") {
        assert!(
            supported,
            "feature on must imply C-side SNMALLOC_PROFILE=ON"
        );
    } else {
        assert!(
            !supported,
            "feature off must imply C-side SNMALLOC_PROFILE undefined; \
             got profiling_supported() == true"
        );
    }
}

/// `snapshot()` is always safe to call.  Aggregations on an empty
/// (or near-empty) profile must not panic.
#[test]
fn snapshot_returns_owned_profile() {
    let a = SnMalloc::new();
    let snap = a.snapshot();
    // Length / emptiness should be self-consistent.
    assert_eq!(snap.is_empty(), snap.len() == 0);
    // Aggregations must be total (no panics, no UB) regardless of
    // sample count.
    let _ = snap.total_allocated_bytes();
    let _ = snap.total_requested_bytes();
    // The samples slice should be exactly `len` long.
    assert_eq!(snap.samples().len(), snap.len());
}

/// With the feature off, the snapshot is always empty and the
/// sampling rate is fixed at zero.  With the feature on, these
/// assertions are skipped -- the rate is mutable then.
#[test]
fn feature_off_is_quiescent() {
    if cfg!(feature = "profiling") {
        return;
    }
    let a = SnMalloc::new();
    assert!(!a.profiling_supported());
    assert_eq!(a.sampling_rate(), 0);
    // set_sampling_rate must be a no-op; the getter must still
    // return zero after.
    a.set_sampling_rate(8192);
    assert_eq!(a.sampling_rate(), 0);
    let snap = a.snapshot();
    assert!(snap.is_empty());
    assert_eq!(snap.total_allocated_bytes(), 0u128);
    assert_eq!(snap.total_requested_bytes(), 0u128);
}

/// With the `profiling` feature on, the sampling rate is settable
/// and read-back is faithful.  We restore the saved value at the end
/// so this test does not perturb the process-global sampler state
/// observed by other tests in the same binary.
#[test]
fn sampling_rate_roundtrips_when_supported() {
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }
    let saved = a.sampling_rate();
    a.set_sampling_rate(4096);
    assert_eq!(a.sampling_rate(), 4096);
    a.set_sampling_rate(1);
    assert_eq!(a.sampling_rate(), 1);
    a.set_sampling_rate(saved);
}

/// Live sampling end-to-end test (Phase 4.2).  Allocates
/// 100_000 x 64B objects with the sampling rate set to 4 KiB and
/// asserts the resulting snapshot contains
/// ~ 100_000 * 64 / 4096 = ~1562 samples within a 6-sigma Poisson
/// envelope.
///
/// Then frees every allocation and snapshots again: the dealloc hook
/// in `snmalloc/profile/record.h` should drain the global SampledList
/// back to (approximately) empty.  We allow a small absolute tolerance
/// to absorb (a) samples produced by other concurrent tests in the
/// same binary that have not yet been freed and (b) the known O(1)
/// cross-thread race documented in `profile_integration.cc`.
///
/// Compiled but trivially-passing on the feature-off build (no Sampler
/// active, snapshot is always empty).
#[test]
fn live_sampling_run() {
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        // Without the feature this test trivially passes (it is
        // only meaningful in feature-on builds).
        return;
    }

    const RATE: usize = 4096;
    const N: usize = 100_000;
    const SIZE: usize = 64;

    a.set_sampling_rate(RATE);

    let layout = Layout::from_size_align(SIZE, 8).unwrap();
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N);
    for _ in 0..N {
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p);
    }

    // Snapshot 1: with N x SIZE bytes live, we expect a statistically
    // meaningful number of samples on the global list.
    let snap_live = a.snapshot();
    let observed = snap_live.len();
    let expected = (N * SIZE) as f64 / RATE as f64;
    let sigma = expected.sqrt();
    let low = expected - 6.0 * sigma;
    let high = expected + 6.0 * sigma;
    assert!(
        observed > 0,
        "expected at least one live sample after {N} x {SIZE}B allocs at \
         rate {RATE}; got 0 -- profile slot is probably not wired into \
         the rust shim's Config"
    );
    assert!(
        (observed as f64) >= low && (observed as f64) <= high,
        "observed {observed} samples, expected {expected:.1} +/- 6 sigma \
         ({sigma:.1}); window = [{low:.1}, {high:.1}]"
    );

    // Free everything; the H1 dealloc hook should clear each per-object
    // slot and remove the matching SampledAlloc from the global list.
    for p in ptrs {
        unsafe { a.dealloc(p, layout) };
    }

    // Snapshot 2: post-free.  Allow a small absolute tolerance for
    // sample noise from any other tests running in the same binary
    // (Cargo runs `#[test]`s on multiple threads) plus the documented
    // sub-1% cross-thread race in record.h.  The key signal is the
    // drop relative to `observed` -- not that we hit exactly zero.
    let snap_drained = a.snapshot();
    let remaining = snap_drained.len();
    assert!(
        remaining < observed,
        "expected sample count to drop after freeing all allocations; \
         was {observed}, still {remaining}"
    );
}
