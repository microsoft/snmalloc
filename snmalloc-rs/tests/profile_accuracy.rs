//! Phase 4.3 integration tests for snmalloc heap profiling.
//!
//! Two halves:
//!
//! 1.  Statistical accuracy of the Poisson sampler.  With a known
//!     workload (N allocations of size B at sampling rate R) the
//!     expected sample count is `lambda = N * B / R`, with standard
//!     deviation `sqrt(lambda)` (Poisson).  We assert observed count
//!     stays inside a 6-sigma envelope and that
//!     `sum(weight)` stays inside the analogous 6-sigma envelope for
//!     the unbiased-sum estimator (variance ~ N * B * R; see the
//!     constants block below for the derivation).  The latter is the
//!     core unbiased-estimator guarantee we ship to users.
//!
//! 2.  Correctness of [`HeapProfile::write_flamegraph`]: every line
//!     parses as `STACK WEIGHT`, every stack is unique (the collapse
//!     step worked), and the sum of folded weights equals the total
//!     under the documented default projection
//!     ([`Weight::Allocated`]).
//!
//! All assertions are skipped (with a `return`, not a `#[ignore]`)
//! when the `profiling` Cargo feature is OFF, because that build
//! cannot produce any samples.  The file still compiles and runs in
//! both configurations -- the no-op path keeps `cargo test --all`
//! green without re-running the build with feature flags.
//!
//! Known caveat: the multi-threaded sampler has a documented O(1/N)
//! per-thread teardown straggler (see Phase 3.4 / `record.h`); the
//! 6-sigma window absorbs it for the workload sizes we use here.

use snmalloc_rs::{SnMalloc, Weight};
use std::alloc::{GlobalAlloc, Layout};
use std::collections::HashSet;
use std::sync::{Arc, Barrier, Mutex, OnceLock};
use std::thread;

/// Process-wide mutex that serialises the heavy accuracy tests in
/// this binary.  Cargo runs `#[test]`s in parallel by default, but
/// the sampling state (rate, global SampledList) is process-global;
/// without serialisation the workloads from different tests would
/// interleave and break the "observed ~ lambda" assertion.
///
/// The lighter `flamegraph_*` tests also take this lock so the
/// snapshots they take aren't polluted by an in-flight accuracy
/// workload.
fn accuracy_lock() -> std::sync::MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

/// Sampling rate used by every test in this file.  Chosen so that the
/// expected sample count is ~1562 for the single-threaded workload --
/// big enough that a 6-sigma window is well-behaved (sigma ~= 39, the
/// window is ~22% of lambda) without being so big that the test runs
/// slowly.
const RATE: usize = 4096;
/// Per-thread allocation count.
const N_PER_THREAD: usize = 100_000;
/// Per-allocation size in bytes.  64 is small enough to live in a
/// dense sizeclass and large enough that ~100k allocations push
/// several MiB of allocator state.
const SIZE: usize = 64;

/// Single-threaded accuracy:
///   - lambda = 100_000 * 64 / 4096 = 1562.5 samples expected
///   - sigma  = sqrt(1562.5)        = ~39.5
///   - 6-sigma window = [1325, 1800] inclusive
///
/// And independently, the unbiased estimator
///   sum(weight) ~ N * SIZE = 6_400_000 bytes
/// must hold to within the analogous 6-sigma envelope.  The variance
/// of the unbiased sum estimator under Poisson sampling at rate R is
///   Var(sum_weight) ~ N * SIZE * R
/// (each sample contributes a geometric-distributed weight of mean R
/// and variance ~R^2; lambda = N*SIZE/R samples in expectation gives
/// total variance lambda * R^2 = N*SIZE*R).  For the constants here:
///   sigma_bytes  = sqrt(6_400_000 * 4096) ~= 161_951
///   relative 1-sigma ~= 2.53% of expected, so a hard 5% bound is only
///   ~1.97 sigma -- that's a one-in-twenty flake under CPU contention,
///   which is exactly the failure mode tracked by 86aj0h83a.  Asserting
///   against the derived 6-sigma envelope ([5_428_293, 7_371_707]) is
///   both more rigorous and dramatically less flaky.
///
/// On the feature-off build this test is a no-op.
#[test]
fn accuracy_single_threaded() {
    let _lock = accuracy_lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let saved = a.sampling_rate();
    // Disable sampling first, baseline-snapshot the existing global
    // SampledList (other tests in this binary may have left samples
    // behind), and only then enable our chosen rate for the workload.
    a.set_sampling_rate(0);
    let baseline = a.snapshot();
    let baseline_count = baseline.len();
    let baseline_requested = baseline.total_requested_bytes();
    drop(baseline);
    a.set_sampling_rate(RATE);

    let layout = Layout::from_size_align(SIZE, 8).unwrap();
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N_PER_THREAD);
    for _ in 0..N_PER_THREAD {
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p);
    }

    let snap = a.snapshot();
    // Subtract the baseline so we're measuring only the samples
    // produced by *this* test's workload.
    let observed = snap.len().saturating_sub(baseline_count);
    let observed_bytes = snap
        .total_requested_bytes()
        .saturating_sub(baseline_requested);

    let expected = (N_PER_THREAD * SIZE) as f64 / RATE as f64;
    let sigma = expected.sqrt();
    let low = expected - 6.0 * sigma;
    let high = expected + 6.0 * sigma;
    assert!(
        observed > 0,
        "got 0 samples after {N_PER_THREAD} x {SIZE}B; profile slot \
         likely not wired into the Rust shim's Config"
    );
    assert!(
        (observed as f64) >= low && (observed as f64) <= high,
        "single-threaded: observed {observed} samples (baseline \
         {baseline_count}), expected {expected:.1} +/- 6 sigma \
         ({sigma:.1}); window = [{low:.1}, {high:.1}]"
    );

    // Unbiased estimator: sum(weight) should be ~ N * SIZE.  Use the
    // requested-bytes view here -- it's exactly sum(weight), no
    // sizeclass scaling -- so the comparison against `N * SIZE` is
    // apples-to-apples regardless of which sizeclass the 64-byte
    // request lands in.
    //
    // The bound is the 6-sigma envelope of the Poisson unbiased-sum
    // estimator: Var(sum_weight) ~ N * SIZE * RATE (see the doc-comment
    // above for the derivation).  This is the statistically honest
    // bound for the chosen (N, SIZE, RATE); a hard percentage cap like
    // 5% works out to only ~1.97 sigma at these constants and flakes
    // under sibling cargo-test CPU contention (ticket 86aj0h83a).
    let expected_bytes_f = (N_PER_THREAD * SIZE) as f64;
    let sigma_bytes = (expected_bytes_f * RATE as f64).sqrt();
    let lo_bytes_f = expected_bytes_f - 6.0 * sigma_bytes;
    let hi_bytes_f = expected_bytes_f + 6.0 * sigma_bytes;
    // Clamp the lower bound at 0 in case 6*sigma exceeds the mean for
    // some future smaller-workload tuning -- u128 would wrap otherwise.
    let lo_bytes: u128 = if lo_bytes_f < 0.0 { 0 } else { lo_bytes_f as u128 };
    let hi_bytes: u128 = hi_bytes_f as u128;
    let expected_bytes = expected_bytes_f as u128;
    assert!(
        observed_bytes >= lo_bytes && observed_bytes <= hi_bytes,
        "single-threaded: sum(weight) = {observed_bytes} bytes \
         (baseline {baseline_requested}), expected {expected_bytes} \
         +/- 6 sigma ({sigma_bytes:.0}); window = [{lo_bytes}, {hi_bytes}]"
    );

    // Clean up.  Drains the global SampledList back toward empty so
    // sibling tests in the same binary aren't polluted.
    for p in ptrs {
        unsafe { a.dealloc(p, layout) };
    }
    a.set_sampling_rate(saved);
}

/// Multi-threaded accuracy: 8 threads x 10k allocations each, same
/// 64-byte size and 4 KiB rate.
///
///   - lambda total = 8 * 10_000 * 64 / 4096 = 1250 expected
///   - sigma        = sqrt(1250) = ~35.4
///   - 6-sigma window = [1037, 1462]
///
/// Per Phase 3.4 there is a known O(1/N) per-thread teardown
/// straggler in the dealloc hook -- a sample produced very late by
/// thread T can still be in flight when T exits and the global list
/// briefly forgets about it.  At N = 80 000 this is well under one
/// sample on average and is absorbed by the 6-sigma window, but we
/// document the source explicitly so the failure mode is recognisable.
///
/// On the feature-off build this test is a no-op.
#[test]
fn accuracy_multi_threaded() {
    let _lock = accuracy_lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    const THREADS: usize = 8;
    const PER_THREAD: usize = 10_000;

    let saved = a.sampling_rate();
    // See `accuracy_single_threaded` for the baseline-subtraction
    // pattern; same rationale applies here.
    a.set_sampling_rate(0);
    let baseline = a.snapshot();
    let baseline_count = baseline.len();
    drop(baseline);
    a.set_sampling_rate(RATE);

    let barrier = Arc::new(Barrier::new(THREADS));
    let mut handles = Vec::with_capacity(THREADS);
    for _ in 0..THREADS {
        let b = barrier.clone();
        handles.push(thread::spawn(move || {
            // Synchronise the start so the live snapshot is taken
            // while all eight threads still hold their allocations.
            b.wait();
            let alloc = SnMalloc::new();
            let layout = Layout::from_size_align(SIZE, 8).unwrap();
            // Stash pointers as usize so the Vec is Send -- raw
            // *mut u8 is not.  We never dereference them on either
            // side, only hand them back to dealloc on the main
            // thread.
            let mut ptrs: Vec<usize> = Vec::with_capacity(PER_THREAD);
            for _ in 0..PER_THREAD {
                let p = unsafe { alloc.alloc(layout) };
                assert!(!p.is_null());
                ptrs.push(p as usize);
            }
            // Don't free yet -- the snapshot below needs the
            // allocations to still be live.  Hand the pointers back
            // out so the main thread can drain them.
            (ptrs, layout)
        }));
    }

    // Briefly busy-wait for the worker threads to allocate; the
    // simplest robust signal is to let them all complete and then
    // snapshot.  The `join` below waits, which is exactly what we
    // want.
    let mut all_ptrs: Vec<(Vec<usize>, Layout)> = Vec::with_capacity(THREADS);
    for h in handles {
        all_ptrs.push(h.join().expect("worker thread panicked"));
    }

    let snap = a.snapshot();
    let observed = snap.len().saturating_sub(baseline_count);
    let expected = (THREADS * PER_THREAD * SIZE) as f64 / RATE as f64;
    let sigma = expected.sqrt();
    let low = expected - 6.0 * sigma;
    let high = expected + 6.0 * sigma;
    assert!(
        observed > 0,
        "got 0 samples after {THREADS} x {PER_THREAD} x {SIZE}B"
    );
    assert!(
        (observed as f64) >= low && (observed as f64) <= high,
        "multi-threaded: observed {observed} samples (baseline \
         {baseline_count}), expected {expected:.1} +/- 6 sigma \
         ({sigma:.1}); window = [{low:.1}, {high:.1}].  See \
         profile_integration.cc for the documented O(1/N) per-thread \
         teardown straggler."
    );

    // Drain the per-thread pointer vectors on the main thread.
    for (ptrs, layout) in all_ptrs {
        for p in ptrs {
            unsafe { a.dealloc(p as *mut u8, layout) };
        }
    }
    a.set_sampling_rate(saved);
}

/// `write_flamegraph` produces a syntactically-valid folded-stack
/// stream over a real-workload snapshot, with no duplicate stacks
/// (the collapse step worked) and a weight-sum that matches
/// `total_allocated_bytes` under the default projection.
///
/// Skipped on the feature-off build (no samples can be produced).
#[test]
fn flamegraph_correctness_over_live_snapshot() {
    let _lock = accuracy_lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let saved = a.sampling_rate();
    a.set_sampling_rate(RATE);

    let layout = Layout::from_size_align(SIZE, 8).unwrap();
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N_PER_THREAD);
    for _ in 0..N_PER_THREAD {
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p);
    }

    let snap = a.snapshot();
    // Require enough samples that the collapsed-format assertions
    // are meaningful.  Below 100 samples we can still inspect
    // syntactic shape, but the "weights match the total" claim
    // becomes too sensitive to Poisson noise to be a useful
    // regression signal.
    assert!(
        snap.len() >= 100,
        "expected at least 100 samples; got {}.  Increase \
         N_PER_THREAD or check that the profile slot is wired in.",
        snap.len()
    );

    // Default (Allocated) projection: the sum of folded line weights
    // must equal HeapProfile::total_allocated_bytes exactly --
    // write_flamegraph and total_allocated_bytes are both derived
    // from the same `sample_weight` helper.
    let mut buf: Vec<u8> = Vec::new();
    snap.write_flamegraph(&mut buf).expect("Vec<u8> write is infallible");
    let text = std::str::from_utf8(&buf).expect("folded format is ASCII");

    let mut seen_stacks: HashSet<String> = HashSet::new();
    let mut sum_weights: u128 = 0;
    let mut line_count: usize = 0;

    for line in text.lines() {
        line_count += 1;
        // "<stack> <weight>".  rsplit so a (forbidden but
        // theoretically possible) ' ' inside the stack rendering
        // wouldn't break the parser.  In practice the stack is hex
        // and ';' only, so the simpler split would also work.
        let mut it = line.rsplitn(2, ' ');
        let weight_str = it.next().expect("trailing weight");
        let stack_str = it.next().expect("leading stack");

        // Weight must be a positive base-10 integer.  Empty stack is
        // allowed (renders as the literal empty string); see
        // `render_stack_key` for why.
        let weight: u128 = weight_str
            .parse()
            .unwrap_or_else(|_| panic!("non-integer weight in line {line:?}"));

        // Frames must be a `;`-separated list of `0x` + 16 hex chars.
        // Allow the empty stack to short-circuit the per-frame check.
        if !stack_str.is_empty() {
            for frame in stack_str.split(';') {
                assert!(
                    frame.starts_with("0x") && frame.len() == 18,
                    "frame {frame:?} in line {line:?} is not a 16-hex code pointer"
                );
                assert!(
                    frame[2..].chars().all(|c| c.is_ascii_hexdigit()),
                    "frame {frame:?} contains a non-hex character"
                );
            }
        }

        // No duplicate stacks: the collapse step must produce a
        // single line per unique frame sequence.
        assert!(
            seen_stacks.insert(stack_str.to_string()),
            "duplicate stack in folded output: {stack_str:?}"
        );

        sum_weights = sum_weights.saturating_add(weight);
    }

    assert!(line_count > 0, "folded output is empty over a >=100-sample snapshot");
    assert!(
        line_count <= snap.len(),
        "unique-stack line count {line_count} cannot exceed sample count {}",
        snap.len()
    );

    let expected = snap.total_allocated_bytes();
    assert_eq!(
        sum_weights, expected,
        "sum of folded weights ({sum_weights}) must equal \
         HeapProfile::total_allocated_bytes ({expected}) under the \
         default Weight::Allocated projection"
    );

    // Explicit Weight::Requested path: sums to total_requested_bytes.
    let mut buf2: Vec<u8> = Vec::new();
    snap.write_flamegraph_with(Weight::Requested, &mut buf2)
        .expect("Vec<u8> write is infallible");
    let text2 = std::str::from_utf8(&buf2).expect("folded format is ASCII");
    let mut sum2: u128 = 0;
    for line in text2.lines() {
        let mut it = line.rsplitn(2, ' ');
        let w: u128 = it.next().unwrap().parse().unwrap();
        let _ = it.next().unwrap();
        sum2 += w;
    }
    assert_eq!(
        sum2,
        snap.total_requested_bytes(),
        "Weight::Requested sum mismatches total_requested_bytes"
    );

    // Cleanup.
    for p in ptrs {
        unsafe { a.dealloc(p, layout) };
    }
    a.set_sampling_rate(saved);
}

/// `write_flamegraph` is a no-op on an empty snapshot.  This is the
/// contract that lets the function be called unconditionally on the
/// profiling-feature-off build, where every snapshot is empty.
#[test]
fn flamegraph_empty_snapshot_writes_nothing() {
    let _lock = accuracy_lock();
    let a = SnMalloc::new();
    let snap = a.snapshot();
    // On the OFF build snap is empty by construction; on the ON
    // build we take a snapshot without first running a workload, so
    // it should also be small (and may even be empty if no test
    // before us in this binary produced samples).  We only assert
    // the empty case here -- otherwise this test would race against
    // sibling tests' sampler state.
    if !snap.is_empty() {
        return;
    }
    let mut buf: Vec<u8> = Vec::new();
    snap.write_flamegraph(&mut buf).expect("infallible");
    assert!(buf.is_empty());
}
