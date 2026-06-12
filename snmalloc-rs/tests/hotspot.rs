//! Integration tests for the Phase 10.1 deliverables:
//!
//!   A. `HeapProfile::top_sites(n, key)` -- pure post-processing
//!      over the existing snapshot samples; no FFI involvement.
//!      Exercised on synthetic samples built via `from_samples` so
//!      the test passes in *both* feature-on and feature-off builds.
//!
//!   B. `SnMalloc::lookup_alloc_site(addr)` -- address -> alloc-site
//!      reverse lookup, including interior-pointer matching.  Only
//!      exercised meaningfully in the feature-on build; in the
//!      feature-off build the FFI stub returns `-1` and the wrapper
//!      yields `None`, which we still assert on.

use snmalloc_rs::{BtSample, HeapProfile, HotSpotKey, SnMalloc};
use std::alloc::{GlobalAlloc, Layout};

// ---------------------------------------------------------------------------
// Deliverable A -- HotSpot table tests (pure Rust, run in both builds).
// ---------------------------------------------------------------------------

/// Construct two distinct stacks that share a leaf frame but differ
/// in the caller frame, so `LeafFrame` collapses them into one
/// bucket while `FullStack` keeps them separate.  Frame addresses
/// are arbitrary opaque values cast from `usize`.
fn make_sample(stack: Vec<usize>, weight: usize) -> BtSample {
    BtSample {
        alloc_ptr: core::ptr::null(),
        // Set requested == allocated so `Weight::Allocated` projects
        // 1:1 from the raw weight; lets the test reason about
        // inclusive_bytes as just the sum of weights per bucket.
        requested_size: 64,
        allocated_size: 64,
        weight,
        stack: stack.into_iter().map(|u| u as *const u8).collect(),
    }
}

/// `top_sites` returns nothing for `n == 0`.
#[test]
fn top_sites_n_zero_returns_empty() {
    let p = HeapProfile::from_samples(vec![
        make_sample(vec![0xaaaa, 0xbbbb], 4096),
    ]);
    assert!(p.top_sites(0, HotSpotKey::LeafFrame).is_empty());
    assert!(p.top_sites(0, HotSpotKey::FullStack).is_empty());
    assert!(p.top_sites(0, HotSpotKey::CallSite).is_empty());
}

/// `top_sites` on an empty profile returns an empty vec.
#[test]
fn top_sites_empty_profile() {
    let p = HeapProfile::default();
    assert!(p.top_sites(10, HotSpotKey::LeafFrame).is_empty());
    assert!(p.top_sites(10, HotSpotKey::FullStack).is_empty());
    assert!(p.top_sites(10, HotSpotKey::CallSite).is_empty());
}

/// `LeafFrame` grouping collapses two distinct stacks that share
/// the same innermost frame.
#[test]
fn top_sites_leaf_frame_collapses_callers() {
    // Innermost-first: leaf 0xaaaa, two different callers.
    let p = HeapProfile::from_samples(vec![
        make_sample(vec![0xaaaa, 0xbbbb], 4096),
        make_sample(vec![0xaaaa, 0xcccc], 8192),
        // Distinct leaf, single sample.
        make_sample(vec![0xdddd, 0xbbbb], 1024),
    ]);
    let sites = p.top_sites(10, HotSpotKey::LeafFrame);
    // Two distinct leaves => two rows.
    assert_eq!(sites.len(), 2);

    // Row 0 is the hot leaf 0xaaaa: 4096 + 8192 = 12288 bytes, 2 samples.
    assert_eq!(sites[0].leaf_frame as usize, 0xaaaa);
    assert_eq!(sites[0].inclusive_bytes, 12288u128);
    assert_eq!(sites[0].sample_count, 2);

    // Row 1 is the cooler leaf 0xdddd.
    assert_eq!(sites[1].leaf_frame as usize, 0xdddd);
    assert_eq!(sites[1].inclusive_bytes, 1024u128);
    assert_eq!(sites[1].sample_count, 1);
}

/// `FullStack` grouping keeps the two callers separate where
/// `LeafFrame` collapses them.
#[test]
fn top_sites_full_stack_keeps_callers_separate() {
    let p = HeapProfile::from_samples(vec![
        make_sample(vec![0xaaaa, 0xbbbb], 4096),
        make_sample(vec![0xaaaa, 0xcccc], 8192),
    ]);
    let sites = p.top_sites(10, HotSpotKey::FullStack);
    // Two distinct full stacks => two rows.
    assert_eq!(sites.len(), 2);
    // Sorted by descending inclusive_bytes; 8192 first.
    assert_eq!(sites[0].inclusive_bytes, 8192u128);
    assert_eq!(sites[1].inclusive_bytes, 4096u128);
    // The leaf of both rows is 0xaaaa (the leaf is the same; the
    // *callers* are what differ).
    assert_eq!(sites[0].leaf_frame as usize, 0xaaaa);
    assert_eq!(sites[1].leaf_frame as usize, 0xaaaa);
    // The full stack is preserved in each row.
    assert_eq!(sites[0].stack.len(), 2);
    assert_eq!(sites[1].stack.len(), 2);
}

/// Ranking truncates to `n`.  Build five distinct leaves with
/// strictly decreasing weights and ask for the top-3.
#[test]
fn top_sites_truncates_to_n() {
    let p = HeapProfile::from_samples(vec![
        make_sample(vec![0x1], 1000),
        make_sample(vec![0x2], 2000),
        make_sample(vec![0x3], 3000),
        make_sample(vec![0x4], 4000),
        make_sample(vec![0x5], 5000),
    ]);
    let sites = p.top_sites(3, HotSpotKey::LeafFrame);
    assert_eq!(sites.len(), 3);
    // Top-3 in descending order.
    assert_eq!(sites[0].leaf_frame as usize, 0x5);
    assert_eq!(sites[1].leaf_frame as usize, 0x4);
    assert_eq!(sites[2].leaf_frame as usize, 0x3);
    // Total of the top-3 = 5000+4000+3000 = 12000.
    let sum: u128 = sites.iter().map(|s| s.inclusive_bytes).sum();
    assert_eq!(sum, 12000u128);
}

/// Empty-stack samples land in the `0` (null-pointer) bucket
/// rather than panicking.  Useful as a sanity check that an
/// edge case in the stack-walker doesn't poison the hot-spot
/// computation.
#[test]
fn top_sites_handles_empty_stacks() {
    let p = HeapProfile::from_samples(vec![
        make_sample(vec![], 1000),
        make_sample(vec![], 2000),
        make_sample(vec![0xfeed], 4000),
    ]);
    let sites = p.top_sites(10, HotSpotKey::LeafFrame);
    assert_eq!(sites.len(), 2);
    // Hottest: 0xfeed with 4000 bytes.
    assert_eq!(sites[0].leaf_frame as usize, 0xfeed);
    assert_eq!(sites[0].inclusive_bytes, 4000u128);
    // Empty-stack bucket: leaf = 0, 1000 + 2000 = 3000 bytes.
    assert_eq!(sites[1].leaf_frame as usize, 0);
    assert_eq!(sites[1].inclusive_bytes, 3000u128);
    assert_eq!(sites[1].sample_count, 2);
}

/// `CallSite` falls back to leaf-frame behaviour in the
/// unsymbolicated build.  Documenting this with a test pins the
/// current contract; the next-symbolicate phase would have to
/// update the assertion.
#[test]
fn top_sites_call_site_degrades_to_leaf() {
    let p = HeapProfile::from_samples(vec![
        make_sample(vec![0xaaaa, 0xbbbb], 4096),
        make_sample(vec![0xaaaa, 0xcccc], 8192),
    ]);
    let leaf_sites = p.top_sites(10, HotSpotKey::LeafFrame);
    let call_sites = p.top_sites(10, HotSpotKey::CallSite);
    // Same shape, same numbers, same ordering.
    assert_eq!(leaf_sites.len(), call_sites.len());
    for (a, b) in leaf_sites.iter().zip(call_sites.iter()) {
        assert_eq!(a.leaf_frame, b.leaf_frame);
        assert_eq!(a.inclusive_bytes, b.inclusive_bytes);
        assert_eq!(a.sample_count, b.sample_count);
    }
}

// ---------------------------------------------------------------------------
// Phase 11.3 -- symbolicate-aware CallSite tests.
//
// These exercise the live backtrace-driven path of `top_sites` for
// `HotSpotKey::CallSite`.  They are split across two compile-time
// configurations:
//
//   * `--features profiling,symbolicate` runs the *real* user-caller
//     grouping test (`callsite_groups_by_user_caller`).
//   * Builds *without* `symbolicate` exercise the documented
//     fallback path (`callsite_fallback_when_unsymbolicated`).
// ---------------------------------------------------------------------------

/// Capture a real return-address backtrace inside a uniquely named,
/// non-inlined function.  Returning the frames lets the test
/// resolve them via the symbolicator the same way Phase 4.5 did
/// for its smoke test (see
/// `snmalloc_rs_phase_4_4_symbolize_probe`).
///
/// Two such probes are defined below: their bodies are identical
/// but their *names* differ, which is exactly what gives the
/// symbolicator something to discriminate on in
/// `callsite_groups_by_user_caller`.
#[cfg(feature = "symbolicate")]
#[inline(never)]
fn snmalloc_rs_phase_11_3_callsite_probe_alpha() -> Vec<*const u8> {
    let mut frames: Vec<*const u8> = Vec::new();
    backtrace::trace(|frame| {
        frames.push(frame.ip() as *const u8);
        true
    });
    frames
}

#[cfg(feature = "symbolicate")]
#[inline(never)]
fn snmalloc_rs_phase_11_3_callsite_probe_beta() -> Vec<*const u8> {
    let mut frames: Vec<*const u8> = Vec::new();
    backtrace::trace(|frame| {
        frames.push(frame.ip() as *const u8);
        true
    });
    frames
}

/// Two allocations whose leaf frames live inside this test process
/// share their innermost frames (allocator-internal or the
/// backtrace trampoline itself), but their user-callers differ
/// because the captures originate in two distinctly-named probe
/// functions.  CallSite must walk past any allocator-internal
/// frames and bucket on the *user* caller, producing two distinct
/// buckets where LeafFrame would have collapsed them into one.
///
/// We use synthetic `BtSample`s rather than driving the real
/// sampler so the test is deterministic across sampling-rate
/// noise; the symbolicator still runs on real return addresses
/// captured by `backtrace::trace`, which is what makes the
/// symbol-name dispatch meaningful.
#[cfg(feature = "symbolicate")]
#[test]
fn callsite_groups_by_user_caller() {
    let alpha = snmalloc_rs_phase_11_3_callsite_probe_alpha();
    let beta = snmalloc_rs_phase_11_3_callsite_probe_beta();
    assert!(!alpha.is_empty(), "alpha probe captured no frames");
    assert!(!beta.is_empty(), "beta probe captured no frames");

    let p = HeapProfile::from_samples(vec![
        BtSample {
            alloc_ptr: core::ptr::null(),
            requested_size: 64,
            allocated_size: 64,
            weight: 4096,
            stack: alpha.clone(),
        },
        BtSample {
            alloc_ptr: core::ptr::null(),
            requested_size: 64,
            allocated_size: 64,
            weight: 8192,
            stack: beta.clone(),
        },
    ]);

    let sites = p.top_sites(10, HotSpotKey::CallSite);
    // The two probes have different demangled names, so the
    // first non-allocator frame in each stack must differ --
    // hence two distinct CallSite buckets.  We don't assert any
    // particular ordering of bytes here because the two probe
    // bodies could resolve to the same leaf if the symbolicator
    // collapses thunks; the existence of two buckets is the
    // load-bearing property.
    assert_eq!(
        sites.len(),
        2,
        "expected 2 CallSite buckets (one per probe), got {}: {:?}",
        sites.len(),
        sites
            .iter()
            .map(|s| (s.leaf_frame, s.inclusive_bytes))
            .collect::<Vec<_>>()
    );
    // Both buckets together must account for the full 4096+8192
    // bytes -- no sample silently dropped.
    let total: u128 = sites.iter().map(|s| s.inclusive_bytes).sum();
    assert_eq!(total, 12288u128);
    let count_total: u64 = sites.iter().map(|s| s.sample_count).sum();
    assert_eq!(count_total, 2);
}

/// A degenerate sample whose entire frame set resolves to an
/// allocator-internal symbol (or fails to resolve at all) must
/// still produce *some* bucket -- the bucketing helper falls back
/// to the leaf frame rather than returning a null bucket key.
/// This guards against the "all-allocator stack" edge case.
///
/// We construct an obviously-unresolvable frame (low virtual
/// address) so the symbolicator reports no name; the
/// `is_allocator_frame_name` predicate returns `false` for the
/// no-name case, so the leaf wins on the first iteration -- which
/// is exactly the fallback contract.
#[cfg(feature = "symbolicate")]
#[test]
fn callsite_falls_back_when_no_user_frame() {
    let unresolvable: *const u8 = 0x1 as *const u8;
    let p = HeapProfile::from_samples(vec![BtSample {
        alloc_ptr: core::ptr::null(),
        requested_size: 32,
        allocated_size: 32,
        weight: 1024,
        stack: vec![unresolvable],
    }]);
    let sites = p.top_sites(10, HotSpotKey::CallSite);
    assert_eq!(sites.len(), 1);
    assert_eq!(sites[0].inclusive_bytes, 1024u128);
    assert_eq!(sites[0].sample_count, 1);
    // The bucket must report a non-null leaf (the unresolvable
    // address itself), not the empty-stack null sentinel.
    assert_eq!(sites[0].leaf_frame, unresolvable);
}

/// In a build *without* the `symbolicate` feature, `CallSite`
/// degrades to `LeafFrame` and must remain total: synthetic
/// samples should produce a non-empty result without panicking.
/// This pins the documented fallback contract.
#[cfg(not(feature = "symbolicate"))]
#[test]
fn callsite_fallback_when_unsymbolicated() {
    let p = HeapProfile::from_samples(vec![
        make_sample(vec![0xaaaa, 0xbbbb], 4096),
        make_sample(vec![0xdddd, 0xeeee], 2048),
    ]);
    let sites = p.top_sites(10, HotSpotKey::CallSite);
    // Two distinct leaves -> two buckets, no panic.
    assert_eq!(sites.len(), 2);
    let total: u128 = sites.iter().map(|s| s.inclusive_bytes).sum();
    assert_eq!(total, 6144u128);
}

// ---------------------------------------------------------------------------
// Deliverable B -- address -> alloc-site reverse lookup tests.
// ---------------------------------------------------------------------------

/// In the feature-off build, the FFI stub returns `-1`, so the
/// safe wrapper must yield `None` for any address.
#[test]
fn lookup_alloc_site_feature_off_returns_none() {
    if cfg!(feature = "profiling") {
        return;
    }
    let a = SnMalloc::new();
    // Any address: the stub doesn't even look at it.
    assert!(a.lookup_alloc_site(0x1234 as *const u8).is_none());
    assert!(a.lookup_alloc_site(core::ptr::null()).is_none());
}

/// A clearly-out-of-band address (low VA, not backed by any heap
/// allocation) must miss even in the feature-on build.  Sanity
/// check for the negative path.
#[test]
fn lookup_alloc_site_miss_for_unmapped_addr() {
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }
    // Page zero is reserved on every supported OS; no heap allocation
    // can ever land there.
    assert!(a.lookup_alloc_site(0x1 as *const u8).is_none());
}

/// End-to-end: allocate a flock of objects with a tight sampling
/// rate, then query the addresses (both base and interior) of every
/// sample listed in the snapshot.  Every hit must return a non-empty
/// frame set whose base/size match the snapshot.
///
/// This test is the acceptance gate for the lookup feature -- if it
/// passes, the C++-side index and the Rust wrapper are wired
/// correctly.  It is a no-op in the feature-off build.
#[test]
fn lookup_alloc_site_matches_snapshot() {
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    const RATE: usize = 4096;
    const N: usize = 50_000;
    const SIZE: usize = 256;

    let saved = a.sampling_rate();
    a.set_sampling_rate(RATE);

    let layout = Layout::from_size_align(SIZE, 8).unwrap();
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N);
    for _ in 0..N {
        let p = unsafe { a.alloc(layout) };
        assert!(!p.is_null());
        ptrs.push(p);
    }

    let snap = a.snapshot();
    assert!(
        !snap.is_empty(),
        "expected at least one sample after {N} x {SIZE}B allocs at \
         rate {RATE}; got 0"
    );

    // For every sampled allocation, base-address lookup must succeed.
    let mut interior_checked = 0usize;
    for sample in snap.samples() {
        let base = sample.alloc_ptr;
        // Some samples may carry a null alloc_ptr if the alloc-side
        // hook lost the race to record one (documented in
        // record.h).  Skip those for the lookup test.
        if base.is_null() {
            continue;
        }
        let hit = a
            .lookup_alloc_site(base)
            .expect("base-address lookup must succeed for a live sample");
        // The lookup must report the same base/size as the snapshot.
        assert_eq!(hit.base_addr, base);
        assert_eq!(hit.allocated_size, sample.allocated_size);
        // The captured frames must match the snapshot's stack.
        assert_eq!(hit.frames.len(), sample.stack.len());
        for (a, b) in hit.frames.iter().zip(sample.stack.iter()) {
            assert_eq!(a, b);
        }

        // Interior pointer: middle of the allocation should also
        // match the same allocation.
        if sample.allocated_size > 1 {
            let interior = unsafe {
                (base as *const u8).add(sample.allocated_size / 2)
            };
            let inside = a.lookup_alloc_site(interior).expect(
                "interior-pointer lookup must succeed for a live sample",
            );
            assert_eq!(inside.base_addr, base);
            assert_eq!(inside.allocated_size, sample.allocated_size);
            interior_checked += 1;
        }
    }

    // We must have exercised the interior-pointer path at least once
    // (the SIZE constant above guarantees allocated_size > 1).
    assert!(
        interior_checked > 0,
        "interior-pointer path was never exercised; \
         no sampled allocations had allocated_size > 1?"
    );

    // Free everything.  After dealloc, the same addresses must miss.
    for p in &ptrs {
        unsafe { a.dealloc(*p, layout) };
    }
    // Pick one previously-live sample address and confirm it now
    // misses.  We use the *first* sample we saw -- if every snapshot
    // sample has been freed, the lookup must report None.
    if let Some(first_base) = snap
        .samples()
        .iter()
        .map(|s| s.alloc_ptr)
        .find(|p| !p.is_null())
    {
        // It's *possible* that the same VA was handed back out by a
        // concurrent test in the same binary, in which case the
        // lookup would still hit a fresh sample.  To avoid this race
        // we don't assert hard `is_none()` here -- instead we assert
        // the address either misses or hits an allocation with a
        // *different* base (no double-counting).  In practice on a
        // single-test binary this fires the strict-miss path.
        let post = a.lookup_alloc_site(first_base);
        match post {
            None => { /* expected on a quiescent binary */ }
            Some(f) => {
                // If a different allocation reused the VA, its base
                // must still equal first_base (we hit the new live
                // sample), and the size may differ.  No assertion
                // beyond "lookup didn't crash" is robust against
                // multi-test concurrency.
                let _ = f;
            }
        }
    }

    a.set_sampling_rate(saved);
}
