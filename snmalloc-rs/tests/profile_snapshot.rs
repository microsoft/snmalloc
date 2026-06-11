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
//! In the ON build, `profiling_supported()` returns `true` and the
//! sampling rate is settable.  However, observing non-zero samples
//! from real allocations requires the underlying C++ Config to carry
//! a `LazyArrayClientMetaDataProvider<ProfileSlot>` -- the default
//! Rust shim (`src/snmalloc/override/rust.cc`) uses the standard
//! `NoClientMetaDataProvider`, so the alloc hook compiles down to a
//! no-op even with `SNMALLOC_PROFILE=ON`.  The `live_sampling_run`
//! test below is therefore `#[ignore]`d with a `// TODO Phase 4.2`
//! marker explaining the required cross-cutting change.

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

/// Live sampling end-to-end test.  Allocates 100_000 x 64B objects
/// with the sampling rate set to 4 KiB and asserts the resulting
/// snapshot contains ~ 100_000 * 64 / 4096 = ~1562 samples within a
/// 6-sigma Poisson envelope.
///
/// Why `#[ignore]` for now
/// ----------------------
///
/// The Rust shim built by `snmalloc-sys/build.rs` is
/// `src/snmalloc/override/rust.cc`, which leaves
/// `SNMALLOC_PROVIDE_OWN_CONFIG` undefined and hence inherits the
/// default `snmalloc::Config` from `src/snmalloc/snmalloc.h`:
///
///     using Config = StandardConfigClientMeta<NoClientMetaDataProvider>;
///
/// The alloc/dealloc hooks in `src/snmalloc/profile/record.h` are
/// gated on `config_has_profile_slot_v<Config>`, which requires the
/// `ClientMeta` to be `LazyArrayClientMetaDataProvider<ProfileSlot>`.
/// With the default config the predicate is `false` and the hooks
/// compile down to no-ops -- so even with `SNMALLOC_PROFILE=ON` we
/// observe zero samples.
///
/// `src/test/func/profile_e2e/profile_e2e.cc` demonstrates the
/// required wiring: define `SNMALLOC_PROVIDE_OWN_CONFIG` and supply
/// a profile-enabled Config before including `snmalloc.h`.  Applying
/// this in `rust.cc` would land the live sampling pipeline behind
/// the Rust crate's `profiling` feature -- but the Phase 4.1 ticket
/// explicitly forbids modifying `rust.cc`.  That change is tracked
/// as Phase 4.2.
///
/// Until then, this test is ignored.  Run it manually with
/// `cargo test --features profiling -- --ignored` once Phase 4.2
/// wires the profile slot into the Rust shim's Config.
#[test]
#[ignore = "Phase 4.2: rust.cc default Config uses NoClientMetaDataProvider, \
            so alloc hook is a compile-time no-op and snapshot() returns 0 \
            samples even with SNMALLOC_PROFILE=ON.  Re-enable once the \
            shim is built with a LazyArrayClientMetaDataProvider<ProfileSlot> \
            Config."]
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

    let snap = a.snapshot();
    let observed = snap.len();

    // Free everything so we don't leak.
    for p in ptrs {
        unsafe { a.dealloc(p, layout) };
    }

    let expected = (N * SIZE) as f64 / RATE as f64;
    let sigma = expected.sqrt();
    let low = expected - 6.0 * sigma;
    let high = expected + 6.0 * sigma;
    assert!(
        (observed as f64) >= low && (observed as f64) <= high,
        "observed {observed} samples, expected {expected:.1} +/- 6 sigma \
         ({sigma:.1}); window = [{low:.1}, {high:.1}]"
    );
}
