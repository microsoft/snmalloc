//! Phase 4.4 integration tests for the snmalloc heap-profile
//! symbolicator.
//!
//! Two halves:
//!
//! 1. Resolve at least half of the unique frames in a live snapshot
//!    to a non-`None` name.  Real snapshots contain a long tail of
//!    addresses inside `libc`, the kernel, the dynamic loader, JIT'd
//!    code, etc.; we deliberately tolerate the unresolved portion
//!    and only assert on the majority case.
//!
//! 2. [`HeapProfile::write_flamegraph_symbolized`] emits valid folded
//!    output: every line parses as `STACK WEIGHT`, every stack is
//!    unique (the collapse step still works after substitution), and
//!    the sum of folded weights equals the equivalent
//!    [`HeapProfile::write_flamegraph`] total under the documented
//!    default projection ([`snmalloc_rs::Weight::Allocated`]).
//!
//! Skipped (with a `return`, not `#[ignore]`) when the `profiling`
//! Cargo feature is OFF -- the file still compiles in that
//! configuration so `cargo test --all` stays green without
//! reconfiguring the build.  The whole file is gated on the
//! `symbolicate` feature; without it the API doesn't exist.

#![cfg(feature = "symbolicate")]

use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};
use std::collections::HashSet;
use std::sync::{Mutex, OnceLock};

/// Per-binary mutex so the symbolizer tests don't race against the
/// `profile_accuracy` tests (which run in the same test process when
/// `cargo test --all` is invoked, but in *different* binaries; the
/// lock here serialises only sibling tests in this file).  The
/// global sampler state is process-wide, but since this binary has
/// only the workload defined here, there's no in-process contention
/// to worry about beyond `cargo test`'s default parallelism within
/// the same crate's tests.
fn lock() -> std::sync::MutexGuard<'static, ()> {
    static L: OnceLock<Mutex<()>> = OnceLock::new();
    L.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

/// Sampling rate and workload chosen to match `profile_accuracy.rs`
/// so the expected sample count is similarly comfortable
/// (lambda ~= 1500).
const RATE: usize = 4096;
const N: usize = 100_000;
const SIZE: usize = 64;

/// At least this fraction of unique frame addresses in a live
/// snapshot must resolve to a non-empty name.  Kernel/JIT/stripped
/// frames legitimately won't resolve; 0.5 is a deliberately
/// conservative floor that has plenty of headroom over the ~0.9
/// rate observed locally on macOS arm64 / Linux x86_64 release builds.
const MIN_RESOLVE_RATIO: f64 = 0.5;

/// `symbolize` over a live snapshot resolves >= MIN_RESOLVE_RATIO of
/// its unique frame addresses to a non-`None` name.
#[test]
fn symbolize_resolves_majority_of_live_frames() {
    let _l = lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

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
        snap.len() >= 100,
        "expected at least 100 samples, got {}; rate or workload too small?",
        snap.len()
    );

    let resolved = snap.symbolize();

    // Build the set of unique frame addresses across the snapshot
    // ourselves, so we can sanity-check that the keyset invariant
    // ("every unique frame is in the map") holds.
    let mut unique: HashSet<*const u8> = HashSet::new();
    for s in snap.samples() {
        for &f in &s.stack {
            unique.insert(f);
        }
    }
    assert!(
        !unique.is_empty(),
        "live snapshot must contain at least one frame"
    );
    for f in &unique {
        assert!(
            resolved.contains_key(f),
            "unique frame {:?} missing from resolved map",
            f
        );
    }
    assert_eq!(
        resolved.len(),
        unique.len(),
        "resolved map has extra keys not present in snapshot"
    );

    let named = resolved.values().filter(|f| f.name.is_some()).count();
    let ratio = named as f64 / resolved.len() as f64;
    assert!(
        ratio >= MIN_RESOLVE_RATIO,
        "only {named}/{} ({:.1}%) unique frames resolved; expected \
         >= {:.0}%",
        resolved.len(),
        ratio * 100.0,
        MIN_RESOLVE_RATIO * 100.0
    );

    for p in ptrs {
        unsafe { a.dealloc(p, layout) };
    }
    a.set_sampling_rate(saved);
}

/// `write_flamegraph_symbolized` produces a syntactically-valid
/// folded-stack stream:
///   - one line per unique resolved stack (no duplicates),
///   - every line parses as `STACK WEIGHT`,
///   - the summed weight equals
///     `HeapProfile::total_allocated_bytes` -- which is also what
///     `write_flamegraph` sums to under the default projection, so
///     the substitution-from-hex-to-name path preserves total weight.
#[test]
fn flamegraph_symbolized_renders_cleanly() {
    let _l = lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

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
    assert!(snap.len() >= 100, "snapshot too small: {}", snap.len());

    let mut buf: Vec<u8> = Vec::new();
    snap.write_flamegraph_symbolized(&mut buf)
        .expect("Vec<u8> write is infallible");
    let text = std::str::from_utf8(&buf).expect("folded format is ASCII");

    let mut seen: HashSet<String> = HashSet::new();
    let mut sum: u128 = 0;
    let mut line_count = 0usize;
    for line in text.lines() {
        line_count += 1;
        // `rsplitn(2, ' ')` -- weight is the trailing whitespace-
        // delimited token.  Anything before is the stack.
        let mut it = line.rsplitn(2, ' ');
        let weight_str = it.next().expect("trailing weight");
        let stack_str = it.next().expect("leading stack");
        let weight: u128 = weight_str
            .parse()
            .unwrap_or_else(|_| panic!("non-integer weight in {line:?}"));

        // Each frame must be either a 16-hex code pointer or a
        // resolved name with no `;` or ` ` inside (the
        // `render_stack_key_symbolized` sanitiser guarantees this).
        for frame in stack_str.split(';') {
            assert!(
                !frame.contains(' '),
                "frame {frame:?} in line {line:?} contains a space"
            );
            if frame.starts_with("0x") {
                assert_eq!(
                    frame.len(),
                    18,
                    "hex frame {frame:?} not 16 digits"
                );
                assert!(
                    frame[2..].chars().all(|c| c.is_ascii_hexdigit()),
                    "hex frame {frame:?} contains a non-hex digit"
                );
            }
            // Names are otherwise arbitrary; we don't enforce a
            // specific demangled form here.
        }

        // No duplicate stacks: the collapse step works even after
        // the hex-to-name substitution.
        assert!(
            seen.insert(stack_str.to_string()),
            "duplicate stack in symbolized folded output: {stack_str:?}"
        );

        sum = sum.saturating_add(weight);
    }
    assert!(line_count > 0, "symbolized folded output is empty");

    // Total weight preservation: the symbolized renderer must sum to
    // the same total as the default projection of
    // `total_allocated_bytes`.  The hex-vs-name substitution operates
    // per-frame on rendering, not per-sample, so this invariant is
    // load-bearing for users who want to swap renderers.
    let expected = snap.total_allocated_bytes();
    assert_eq!(
        sum, expected,
        "symbolized folded weight sum ({sum}) must equal \
         total_allocated_bytes ({expected})"
    );

    for p in ptrs {
        unsafe { a.dealloc(p, layout) };
    }
    a.set_sampling_rate(saved);
}
