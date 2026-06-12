//! Integration tests for the Phase 9.5 allocation-lifetime histogram.
//!
//! [`snmalloc_rs::HeapProfile::lifetime_histogram`] returns a snapshot
//! of a process-wide log2-spaced histogram of sampled-allocation
//! lifetimes (in nanoseconds).  Bucket `i` covers lifetimes with
//! `floor(log2(lifetime_ns)) == i`; bucket 31 saturates for very
//! long-lived allocations.
//!
//! These tests are written so they compile and run in BOTH the
//! `profiling`-feature-on and -off builds.  In the off build the
//! histogram is necessarily all-zero (no sample ever fires), so the
//! tests reduce to a basic API smoke test.  In the on build we
//! exercise the alloc -> sleep -> dealloc path with a low sampling
//! rate and assert that the corresponding log2 bucket(s) accumulate
//! the expected counts.

use snmalloc_rs::{HeapProfile, SnMalloc};
use std::alloc::{GlobalAlloc, Layout};
use std::thread;
use std::time::Duration;

/// Number of buckets exposed by the FFI / Rust mirror (must match
/// `SN_RUST_PROFILE_LIFETIME_BUCKETS` in `snmalloc-sys`).
const N_BUCKETS: usize = snmalloc_sys::SN_RUST_PROFILE_LIFETIME_BUCKETS;

/// `lifetime_histogram()` must always be callable and return exactly
/// `N_BUCKETS` u64 entries.  When the `profiling` feature is off the
/// histogram is necessarily all-zero.
#[test]
fn lifetime_histogram_api_smoke() {
    let buckets = HeapProfile::lifetime_histogram();
    assert_eq!(buckets.len(), N_BUCKETS, "fixed-size histogram length");

    let a = SnMalloc::new();
    if !a.profiling_supported() {
        assert!(
            buckets.iter().all(|&b| b == 0),
            "feature-off build must report an all-zero histogram"
        );
    }
}

/// Helper: compute the inclusive log2 bucket index for a known
/// lifetime in nanoseconds, mirroring the C++ `bucket_for` helper.
fn bucket_for(ns: u64) -> usize {
    if ns <= 1 {
        return 0;
    }
    let b = 63 - (ns.leading_zeros() as usize);
    if b >= N_BUCKETS {
        N_BUCKETS - 1
    } else {
        b
    }
}

/// End-to-end alloc -> sleep -> dealloc test.  With a 1-byte sampling
/// rate every allocation fires a sample, so even a single 1 MiB alloc
/// is guaranteed to land on the SampledList.  After a ~50 ms sleep
/// and dealloc we expect the bucket for log2(50 ms in ns) to gain
/// at least one count.  log2(50_000_000) ~ 25.5, so the bump should
/// land in bucket 25 or 26.
#[test]
fn lifetime_histogram_observes_sleep_window() {
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        // Trivially passes on the feature-off build.
        return;
    }

    let saved_rate = a.sampling_rate();
    // Force every allocation to fire a sample so the test is
    // deterministic.  The sampler internally bootstraps an initial
    // countdown drawn from Exp(rate), but at rate=1 the next draw is
    // always 1 byte so any single allocation crosses the threshold.
    a.set_sampling_rate(1);

    // Window the histogram around the operation under test so other
    // allocations from cargo's test infrastructure don't perturb the
    // assertion.
    let before = HeapProfile::lifetime_histogram();

    // 1 MiB allocation -- large enough that it almost certainly
    // fires a sample on its own under any sampling rate, and small
    // enough that the underlying mmap is cheap.
    let layout = Layout::from_size_align(1 << 20, 64).unwrap();
    let ptr = unsafe { a.alloc(layout) };
    assert!(!ptr.is_null(), "1 MiB alloc must succeed");

    // Sleep at least 50 ms.  thread::sleep guarantees a lower bound
    // on the wall-clock delay; the actual elapsed time may be larger
    // under loaded CI runners, which only pushes the lifetime into a
    // *higher* bucket -- still strictly greater than the lower-bound
    // bucket asserted below.
    thread::sleep(Duration::from_millis(50));

    unsafe { a.dealloc(ptr, layout) };

    let after = HeapProfile::lifetime_histogram();
    a.set_sampling_rate(saved_rate);

    // Compute the per-bucket delta over the window.
    let mut delta = [0u64; N_BUCKETS];
    for i in 0..N_BUCKETS {
        delta[i] = after[i].saturating_sub(before[i]);
    }
    let total: u64 = delta.iter().sum();

    assert!(
        total >= 1,
        "expected at least one lifetime bump across the 50ms window; \
         got per-bucket delta {:?}",
        delta
    );

    // 50 ms = 5e7 ns, log2(5e7) ~= 25.6.  Any bucket >= 25 satisfies
    // "at least 50 ms"; we allow some slack for slow CI runners that
    // sleep significantly longer.
    let min_expected_bucket = bucket_for(50_000_000);
    let max_bucket_with_count = (0..N_BUCKETS)
        .rev()
        .find(|&i| delta[i] > 0)
        .expect("at least one bucket must have a non-zero delta");
    assert!(
        max_bucket_with_count >= min_expected_bucket,
        "expected a bump in bucket >= {} (>= 50 ms); highest observed = {} \
         (delta = {:?})",
        min_expected_bucket,
        max_bucket_with_count,
        delta
    );
}

/// Sanity check the helper-side `bucket_for` arithmetic matches the
/// documented contract: powers of two land on their log2 exponent,
/// and very-long lifetimes saturate at the last bucket.
#[test]
fn bucket_for_matches_log2() {
    assert_eq!(bucket_for(0), 0);
    assert_eq!(bucket_for(1), 0);
    assert_eq!(bucket_for(2), 1);
    assert_eq!(bucket_for(3), 1);
    assert_eq!(bucket_for(4), 2);
    assert_eq!(bucket_for(8), 3);
    assert_eq!(bucket_for(1024), 10);
    // Saturate.
    assert_eq!(bucket_for(u64::MAX), N_BUCKETS - 1);
    assert_eq!(bucket_for(1u64 << 31), N_BUCKETS - 1);
    assert_eq!(bucket_for(1u64 << 62), N_BUCKETS - 1);
}
