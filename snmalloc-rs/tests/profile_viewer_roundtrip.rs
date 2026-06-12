//! Phase 4.6 -- viewer round-trip tests for the folded-stack output
//! emitted by [`HeapProfile::write_flamegraph`].
//!
//! This is a **test-only** phase: no new public API on
//! [`HeapProfile`] / [`SnMalloc`] is added, and the wrapper in
//! `src/profile.rs` is not touched.  The point is to assert that the
//! output we ship is consumable by two real viewers in the ecosystem:
//!
//! 1.  [`inferno`](https://github.com/jonhoo/inferno) -- the pure-Rust
//!     port of Brendan Gregg's `flamegraph.pl`.  We can drive it in
//!     process here as a `dev-dependency` and have it render the
//!     folded bytes into an SVG, which we then sanity-check.
//! 2.  [speedscope](https://www.speedscope.app/) -- a browser/wasm
//!     viewer we can't actually run in CI, but whose
//!     [`importable text format`][1] is defined by a very small
//!     regex.  We re-parse our output with the same regex and assert
//!     >=95% of lines parse, which is the conformance contract
//!     speedscope itself uses.
//!
//! [1]: https://github.com/jlfwong/speedscope/wiki/Importing-from-custom-sources
//!
//! There are also two structural invariants that aren't really about
//! viewers per se but are easiest to express in the same file:
//!
//! 3.  `round_trip_weight_invariance` -- the sum of weights in the
//!     folded output must equal [`HeapProfile::total_allocated_bytes`].
//!     This is a regression guard for the Phase 4.3 BTreeMap collapse
//!     step: if collapsing ever started dropping or double-counting a
//!     stack, the totals would silently disagree.
//! 4.  `empty_snapshot_viewer_safety` -- on an empty profile,
//!     `write_flamegraph` writes nothing, and feeding that empty
//!     stream to `inferno` must surface a clean `Err` rather than a
//!     panic.  The OFF-build path runs through here too, since every
//!     snapshot is empty under that configuration.
//!
//! Skipping pattern
//! ----------------
//!
//! The "real-workload" tests early-return (`return`, not `#[ignore]`)
//! when `profiling_supported()` is false, mirroring
//! `profile_accuracy.rs`.  That keeps `cargo test --all` green in the
//! feature-off build without needing a separate test binary.

// The workload-driving helpers (and the SnMalloc / GlobalAlloc imports
// they need) are only referenced from `#[cfg(feature = "profiling")]`
// tests.  Gating them avoids dead-code warnings in the feature-off
// build, where every workload test is replaced by a no-op compile path.
#[cfg(feature = "profiling")]
mod workload {
    use snmalloc_rs::SnMalloc;
    use std::alloc::{GlobalAlloc, Layout};
    use std::sync::{Mutex, MutexGuard, OnceLock};

    /// Sampling rate used by every workload-driving test in this file.
    /// 512-byte mean interval (vs the 4 KiB used in `profile_accuracy.rs`)
    /// keeps the per-test workload to ~5k allocations: easily enough to
    /// satisfy the >=50-sample precondition with multiple sigma of
    /// headroom for Poisson noise, while staying lightweight enough that
    /// these tests don't compete heavily for CPU with
    /// `profile_accuracy.rs` running in a sibling test binary (`cargo
    /// test --all` parallelises binaries by default).  CPU contention
    /// matters because Phase 4.3's `accuracy_single_threaded` has a
    /// tight 5%-of-(N*SIZE) tolerance on `sum(weight)` that is already
    /// pre-existing flaky under heavy parallel load; we keep our
    /// footprint modest to minimise that interaction.  At
    /// lambda = 5000 * 64 / 512 = 625 expected samples the >=50-sample
    /// precondition has many sigma of margin.
    pub const RATE: usize = 512;
    /// Allocations per workload.  At `RATE = 512` this produces ~625
    /// samples on average -- well above the 50-sample floor Phase 4.6
    /// requires for the inferno round-trip while staying small enough
    /// that the total work for this test binary is a fraction of a
    /// second.
    pub const N_ALLOCS: usize = 5_000;
    /// Per-allocation size.  Small enough to land in a dense sizeclass.
    pub const SIZE: usize = 64;

    /// Process-wide mutex matching the one in `profile_accuracy.rs`.
    /// Cargo runs `#[test]`s in parallel by default, but the sampler
    /// state (rate + global SampledList) is process-global, so a
    /// workload-driving test that doesn't take this lock can be polluted
    /// by sibling tests in the same binary.  We intentionally do not
    /// share the lock with `profile_accuracy.rs` (each integration test
    /// compiles to its own binary), so this is a fresh `OnceLock` here.
    pub fn workload_lock() -> MutexGuard<'static, ()> {
        static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
        LOCK.get_or_init(|| Mutex::new(()))
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    /// Run a workload large enough to land at least `min_samples`
    /// samples in the snapshot.  Returns the snapshot and a "cleanup"
    /// closure that the caller must invoke before returning (to drain
    /// the global SampledList for sibling tests).  Panics if the
    /// snapshot comes back with fewer than `min_samples` samples after
    /// the workload, since that means either the profile slot isn't
    /// wired in or the sampler is mis-calibrated -- in either case the
    /// rest of the test would produce a misleading green.
    ///
    /// `min_samples` should be at least 50 per the Phase 4.6 spec.
    pub fn run_workload(
        min_samples: usize,
    ) -> (snmalloc_rs::HeapProfile, Box<dyn FnOnce()>) {
        let a = SnMalloc::new();
        let saved = a.sampling_rate();
        a.set_sampling_rate(RATE);

        let layout = Layout::from_size_align(SIZE, 8).expect("valid layout");
        let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N_ALLOCS);
        for _ in 0..N_ALLOCS {
            // SAFETY: layout is non-zero and aligned; we feed every
            // pointer back into dealloc with the same layout below.
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

        // Defer the dealloc loop and rate restore to a closure: the
        // caller wants to do its assertions against the snapshot
        // *first*, while the allocations are still live and stable.
        let cleanup = Box::new(move || {
            let a = SnMalloc::new();
            for p in ptrs {
                // SAFETY: each `p` came from `alloc(layout)` above and
                // has not been freed.
                unsafe { a.dealloc(p, layout) };
            }
            a.set_sampling_rate(saved);
        });

        (snap, cleanup)
    }
}

/// Round-trip test 1: hand our folded-stack output to inferno and
/// confirm it produces an SVG.  We only require *structural* validity
/// of the SVG -- a `<svg` prefix and at least one `<g` group node
/// (one per stack frame in the rendered flamegraph).  Pixel-perfect
/// output stability isn't something we control: inferno can change
/// its rendering across point releases.
///
/// inferno crate version is pinned in `Cargo.toml`'s `[dev-dependencies]`.
#[cfg(feature = "profiling")]
#[test]
fn inferno_roundtrip() {
    let _lock = workload::workload_lock();
    let a = snmalloc_rs::SnMalloc::new();
    if !a.profiling_supported() {
        // Belt-and-braces -- the cfg above already gates this, but
        // catching it at runtime too means a build with `--features
        // profiling` against an OFF C++ build degrades gracefully
        // rather than spuriously panicking.
        return;
    }

    let (snap, cleanup) = workload::run_workload(50);

    // Capture our folded-stack output into an in-memory buffer so the
    // round-trip stays entirely in process.  inferno consumes
    // anything that implements `BufRead`; a `&[u8]` does, via `Read`'s
    // wrapper.
    let mut folded: Vec<u8> = Vec::new();
    snap.write_flamegraph(&mut folded)
        .expect("Vec<u8> write is infallible");
    assert!(
        !folded.is_empty(),
        "folded output unexpectedly empty after a >=50-sample snapshot"
    );

    let mut svg: Vec<u8> = Vec::new();
    let mut opts = inferno::flamegraph::Options::default();
    // `Options::default()` is fine for round-trip purposes; we are not
    // asserting on title / colour / font.  Document the intent so a
    // reader doesn't think we've forgotten to configure something
    // important.
    let _ = &mut opts;

    let cursor = std::io::Cursor::new(&folded[..]);
    inferno::flamegraph::from_reader(&mut opts, cursor, &mut svg)
        .expect("inferno must accept the folded stream we produced");

    let svg_text = std::str::from_utf8(&svg).expect("inferno emits UTF-8 SVG");

    assert!(
        svg_text.contains("<svg"),
        "inferno output missing <svg root tag; first 200 chars: {:?}",
        &svg_text.chars().take(200).collect::<String>()
    );
    // Inferno emits one `<g>` element per stack frame.  The opening
    // tag may be `<g>` (no attrs) or `<g ...>` (with attrs) depending
    // on the inferno point release; both forms count as a group
    // node.  A "no stacks" fallback would emit zero `<g` openers.
    let has_group = svg_text.contains("<g>") || svg_text.contains("<g ");
    assert!(
        has_group,
        "inferno output missing any <g> stack-frame node; this usually \
         means the folded stream rendered to a 'no stacks' fallback. \
         First 400 chars of SVG: {:?}",
        &svg_text.chars().take(400).collect::<String>()
    );

    cleanup();
}

/// Round-trip test 2: speedscope's "Brendan Gregg's collapsed stack
/// format" importer parses each line with the regex `^([^\s]+) (\d+)$`
/// (the source is the [`speedscope` wiki page][1]).  We apply the
/// same regex here and require at least 95% of non-empty output lines
/// to match.
///
/// We don't require 100% because the documented contract of
/// [`HeapProfile::write_flamegraph`] permits an empty-stack rendering
/// (an `[unknown]` bar) which would print as ` <weight>` -- with a
/// leading space, no leading non-whitespace token, and therefore
/// failing the speedscope regex.  In practice empty stacks are very
/// rare on a Phase 3 build (the stack-walker reliably returns at
/// least the call site) but the contract is conservative.
///
/// [1]: https://github.com/jlfwong/speedscope/wiki/Importing-from-custom-sources
#[cfg(feature = "profiling")]
#[test]
fn speedscope_folded_import() {
    let _lock = workload::workload_lock();
    let a = snmalloc_rs::SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let (snap, cleanup) = workload::run_workload(50);

    let mut folded: Vec<u8> = Vec::new();
    snap.write_flamegraph(&mut folded)
        .expect("Vec<u8> write is infallible");
    let text = std::str::from_utf8(&folded).expect("folded format is ASCII");

    // Reimplement speedscope's importer regex by hand to avoid pulling
    // in the `regex` crate as a dev-dependency.  The contract is
    // exactly:
    //
    //   ^([^\s]+) (\d+)$
    //
    // i.e. one or more non-whitespace chars (the stack), a single
    // ASCII space, one or more ASCII digits (the weight), end of
    // line.  We treat the regex as anchored: any deviation (extra
    // whitespace, trailing chars, multi-space, empty stack) is a
    // non-match.
    fn speedscope_matches(line: &str) -> bool {
        // Splitting on the *last* space lets a (theoretical) space
        // inside the stack rendering still parse -- but since our
        // stack is hex + ';' it never contains whitespace, so a
        // simpler split would also work.  rsplitn is just defensive.
        let mut it = line.rsplitn(2, ' ');
        let weight = match it.next() {
            Some(s) if !s.is_empty() => s,
            _ => return false,
        };
        let stack = match it.next() {
            Some(s) => s,
            None => return false,
        };
        // Stack must be one or more non-whitespace chars.
        if stack.is_empty() || stack.chars().any(|c| c.is_whitespace()) {
            return false;
        }
        // Weight must be one or more ASCII digits, nothing else.
        weight.chars().all(|c| c.is_ascii_digit()) && !weight.is_empty()
    }

    let mut total: usize = 0;
    let mut matched: usize = 0;
    for line in text.lines() {
        // Skip truly empty lines -- speedscope ignores them.  Our
        // `write_flamegraph` never emits them, but defensive parsing
        // protects against future format tweaks.
        if line.is_empty() {
            continue;
        }
        total += 1;
        if speedscope_matches(line) {
            matched += 1;
        }
    }
    assert!(total > 0, "folded output empty over a >=50-sample snapshot");

    // 95% conformance.  Use integer arithmetic to avoid floating-point
    // surprises: `matched * 100 >= total * 95`.
    assert!(
        matched.saturating_mul(100) >= total.saturating_mul(95),
        "only {}/{} folded lines ({}%) match speedscope's importer \
         regex `^([^\\s]+) (\\d+)$`; required >= 95%",
        matched,
        total,
        (matched.saturating_mul(100)) / total.max(1)
    );

    cleanup();
}

/// Regression guard for the Phase 4.3 BTreeMap collapse step.  If
/// collapsing ever started dropping or double-counting a stack, the
/// folded weight sum would silently disagree with
/// [`HeapProfile::total_allocated_bytes`].  Phase 4.3 already covers
/// this on synthetic samples (`flamegraph_weight_sum_matches_total_allocated`
/// in `src/profile.rs`); we re-assert it here over a real-workload
/// snapshot, both because the unit test only sees two samples and
/// because Phase 4.6's whole point is to harden the
/// production-shape output.
#[cfg(feature = "profiling")]
#[test]
fn round_trip_weight_invariance() {
    let _lock = workload::workload_lock();
    let a = snmalloc_rs::SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let (snap, cleanup) = workload::run_workload(50);

    let mut folded: Vec<u8> = Vec::new();
    snap.write_flamegraph(&mut folded)
        .expect("Vec<u8> write is infallible");
    let text = std::str::from_utf8(&folded).expect("folded format is ASCII");

    let mut sum: u128 = 0;
    for line in text.lines() {
        // "<stack> <weight>".  rsplit so any (forbidden but
        // theoretically possible) inner space wouldn't break parsing.
        let mut it = line.rsplitn(2, ' ');
        let weight: u128 = it
            .next()
            .expect("trailing weight")
            .parse()
            .unwrap_or_else(|_| panic!("non-integer weight in line {:?}", line));
        let _stack = it.next().expect("leading stack");
        sum = sum.saturating_add(weight);
    }

    assert_eq!(
        sum,
        snap.total_allocated_bytes(),
        "sum of folded weights does not match HeapProfile::total_allocated_bytes; \
         the BTreeMap collapse step in write_flamegraph dropped or duplicated a stack"
    );

    cleanup();
}

/// Safety contract for both viewers on an empty input:
///
/// - [`HeapProfile::write_flamegraph`] on an empty profile writes zero
///   bytes and returns `Ok(())` (this is the documented no-op
///   contract).
/// - inferno's `from_reader` on the resulting empty stream must
///   produce an `Err` rather than a panic; specifically inferno
///   rejects an empty input with an error like "no stack counts found".
///
/// Both branches matter for the OFF build path, where every snapshot
/// is empty by construction.  This test is therefore intentionally
/// *not* gated on the `profiling` feature -- it runs in both
/// configurations.  We construct a default `HeapProfile` directly so
/// the test doesn't depend on the sampler at all.
#[test]
fn empty_snapshot_viewer_safety() {
    let p = snmalloc_rs::HeapProfile::default();
    assert!(p.is_empty());

    let mut folded: Vec<u8> = Vec::new();
    p.write_flamegraph(&mut folded)
        .expect("empty profile write is infallible");
    assert!(
        folded.is_empty(),
        "empty profile must produce zero-length folded output; got {} bytes",
        folded.len()
    );

    // Inferno is only on the dev-dependency path; we still run this
    // assertion under both feature configs because dev-deps don't
    // care about feature gates.  inferno::from_reader on a zero-byte
    // input is contractually required to return Err (it has nothing
    // to render); the key property here is that it does so without
    // panicking, which would crash the entire test binary.
    let mut svg: Vec<u8> = Vec::new();
    let mut opts = inferno::flamegraph::Options::default();
    let cursor = std::io::Cursor::new(&folded[..]);
    let result = inferno::flamegraph::from_reader(&mut opts, cursor, &mut svg);
    assert!(
        result.is_err(),
        "inferno should reject an empty folded stream with an Err, \
         not silently produce an SVG; got Ok(()) with {} bytes of SVG",
        svg.len()
    );
}
