//! Phase 6.1 -- integration tests for the pprof Profile encoder
//! ([`HeapProfile::write_pprof`]).
//!
//! Three tests:
//!
//! 1.  `write_pprof_smoke` -- run a live workload, write to a
//!     `Vec<u8>`, and check the bytes parse back through our minimal
//!     in-test pprof decoder.  The encoded form is **not** gzipped
//!     (see `src/pprof.rs` for the rationale), so we explicitly
//!     assert the first byte is *not* the gzip magic 0x1f.  Gated on
//!     the `profiling` feature.
//! 2.  `write_pprof_empty_snapshot` -- on a default-constructed
//!     [`HeapProfile`], write_pprof emits a valid but small Profile
//!     containing the two sample-type axes and the
//!     `default_sample_type` hint.  Runs in both feature configs.
//! 3.  `pprof_total_weight_matches_total_allocated_bytes` --
//!     sum(sample.value[1]) over the encoded Profile must equal
//!     [`HeapProfile::total_allocated_bytes`] under
//!     [`Weight::Allocated`].  Gated on the `profiling` feature.
//!
//! Why an in-test decoder?  Pulling in `prost`/`prost-types` as a
//! dev-dependency just for round-trip validation would compile half
//! the prost ecosystem; a 60-line walker covers exactly the field
//! shapes our encoder emits.

#![cfg(feature = "profiling")]

use snmalloc_rs::{HeapProfile, SnMalloc, Weight};
use std::alloc::{GlobalAlloc, Layout};
use std::sync::{Mutex, MutexGuard, OnceLock};

// =========================================================================
// Workload helpers -- match the shape used in
// `tests/profile_viewer_roundtrip.rs`.
// =========================================================================

const RATE: usize = 512;
const N_ALLOCS: usize = 5_000;
const SIZE: usize = 64;

/// Process-wide mutex so this binary doesn't trip on its sibling
/// `profile_accuracy.rs` / `profile_viewer_roundtrip.rs` workloads
/// running in parallel.  Each integration test compiles to its own
/// binary, so this lock is local to this binary -- which is the
/// usual cargo-test pattern.
fn workload_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

/// Run a workload, take a snapshot, and return it along with a
/// cleanup closure that frees the allocations and restores the
/// previous sampling rate.  Panics if fewer than `min_samples` were
/// captured.
fn run_workload(min_samples: usize) -> (HeapProfile, Box<dyn FnOnce()>) {
    let a = SnMalloc::new();
    let saved = a.sampling_rate();
    a.set_sampling_rate(RATE);

    let layout = Layout::from_size_align(SIZE, 8).expect("valid layout");
    let mut ptrs: Vec<*mut u8> = Vec::with_capacity(N_ALLOCS);
    for _ in 0..N_ALLOCS {
        // SAFETY: layout is non-zero, every pointer is fed back to
        // dealloc in the cleanup closure.
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

    let cleanup = Box::new(move || {
        let a = SnMalloc::new();
        for p in ptrs {
            // SAFETY: each `p` came from `alloc(layout)` above and
            // has not been freed yet.
            unsafe { a.dealloc(p, layout) };
        }
        a.set_sampling_rate(saved);
    });

    (snap, cleanup)
}

// =========================================================================
// Minimal pprof decoder.  Walks only the fields our encoder emits.
// =========================================================================

const WIRE_TYPE_VARINT: u32 = 0;
const WIRE_TYPE_LEN: u32 = 2;

/// Decode one u64 varint from `buf`, returning (value, bytes_consumed).
fn read_varint(buf: &[u8]) -> (u64, usize) {
    let mut value: u64 = 0;
    let mut shift: u32 = 0;
    for (i, &b) in buf.iter().enumerate() {
        value |= ((b & 0x7f) as u64) << shift;
        if b & 0x80 == 0 {
            return (value, i + 1);
        }
        shift += 7;
        assert!(shift < 64, "varint overflow at offset {}", i);
    }
    panic!("truncated varint");
}

/// Generic walk of a message buffer.  Calls `visit` for every top-level
/// field, passing the field number, wire type, and (for length-
/// delimited fields) the sub-payload slice.  Returns nothing; the
/// callback accumulates into its own state.
fn walk<F: FnMut(u32, u32, &[u8])>(buf: &[u8], mut visit: F) {
    let mut i: usize = 0;
    while i < buf.len() {
        let (tag, n) = read_varint(&buf[i..]);
        i += n;
        let field = (tag >> 3) as u32;
        let wire = (tag & 0x7) as u32;
        match wire {
            WIRE_TYPE_LEN => {
                let (len, n) = read_varint(&buf[i..]);
                i += n;
                let end = i + len as usize;
                visit(field, wire, &buf[i..end]);
                i = end;
            }
            WIRE_TYPE_VARINT => {
                let start = i;
                let (_v, n) = read_varint(&buf[i..]);
                i += n;
                visit(field, wire, &buf[start..start + n]);
            }
            _ => panic!("unsupported wire type {} for field {}", wire, field),
        }
    }
}

/// Decoded view of the *parts of the* pprof Profile we care about
/// validating.
#[derive(Default, Debug)]
struct DecodedProfile {
    /// Number of `sample_type` ValueType records.
    sample_type_count: usize,
    /// Number of `sample` records.
    sample_count: usize,
    /// Number of `location` records.
    location_count: usize,
    /// Number of `function` records.
    function_count: usize,
    /// String table entries in insertion order.
    strings: Vec<String>,
    /// Sum of every `Sample.value[1]` (the `alloc_space` axis).
    alloc_space_total: i64,
    /// `default_sample_type` (string-table index), if present.
    default_sample_type: Option<i64>,
    /// Total count axis (sum of `value[0]`).  Should equal
    /// `sample_count` for our encoder.
    alloc_objects_total: i64,
}

fn decode_profile(buf: &[u8]) -> DecodedProfile {
    let mut out = DecodedProfile::default();
    walk(buf, |field, wire, payload| {
        match (field, wire) {
            (1, WIRE_TYPE_LEN) => out.sample_type_count += 1,
            (2, WIRE_TYPE_LEN) => {
                out.sample_count += 1;
                // Sample.value is a packed int64 at field 2.
                let mut values: Vec<i64> = Vec::new();
                walk(payload, |sf, sw, sp| {
                    if sf == 2 && sw == WIRE_TYPE_LEN {
                        let mut j = 0usize;
                        while j < sp.len() {
                            let (v, n) = read_varint(&sp[j..]);
                            j += n;
                            values.push(v as i64);
                        }
                    }
                });
                if let Some(v) = values.first() {
                    out.alloc_objects_total += *v;
                }
                if let Some(v) = values.get(1) {
                    out.alloc_space_total += *v;
                }
            }
            (4, WIRE_TYPE_LEN) => out.location_count += 1,
            (5, WIRE_TYPE_LEN) => out.function_count += 1,
            (6, WIRE_TYPE_LEN) => {
                out.strings
                    .push(String::from_utf8_lossy(payload).into_owned());
            }
            (14, WIRE_TYPE_VARINT) => {
                let (v, _) = read_varint(payload);
                out.default_sample_type = Some(v as i64);
            }
            _ => {}
        }
    });
    out
}

// =========================================================================
// Tests
// =========================================================================

/// Smoke test: live snapshot + write_pprof + decode round-trip.
#[test]
fn write_pprof_smoke() {
    let _lock = workload_lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        // Belt-and-braces: the `cfg(feature = "profiling")` at the
        // top of the file already gates this binary, but if someone
        // turns the feature on against an OFF C++ build the early
        // return is the documented graceful-degradation path.
        return;
    }

    let (snap, cleanup) = run_workload(50);

    let mut buf: Vec<u8> = Vec::new();
    snap.write_pprof(&mut buf, Weight::Allocated)
        .expect("Vec<u8> write is infallible");
    assert!(!buf.is_empty(), "pprof bytes unexpectedly empty");

    // We intentionally do not gzip; the first byte must NOT be the
    // gzip magic 0x1f.  (The first byte should be the tag byte for
    // field 1 sample_type -- `(1 << 3) | 2 = 0x0a`.)
    assert_ne!(
        buf[0], 0x1f,
        "pprof output unexpectedly looks gzipped; first byte = 0x{:02x}",
        buf[0]
    );
    assert_eq!(
        buf[0], 0x0a,
        "expected first byte = 0x0a (field 1 sample_type tag); got 0x{:02x}",
        buf[0]
    );

    let decoded = decode_profile(&buf);
    assert_eq!(
        decoded.sample_type_count, 2,
        "must emit exactly two sample_type axes; got {}",
        decoded.sample_type_count
    );
    assert_eq!(
        decoded.sample_count,
        snap.len(),
        "encoded sample count ({}) must match HeapProfile::len ({})",
        decoded.sample_count,
        snap.len()
    );
    assert!(
        decoded.function_count > 0,
        "must emit at least one Function record"
    );
    assert!(
        decoded.location_count > 0,
        "must emit at least one Location record"
    );
    // String table is non-empty and slot 0 is "".
    assert!(!decoded.strings.is_empty());
    assert_eq!(decoded.strings[0], "");
    // Required sample-type axis names live in the string table.
    for needle in &["alloc_objects", "count", "alloc_space", "bytes"] {
        assert!(
            decoded.strings.iter().any(|s| s == needle),
            "string table missing required entry {:?}; got: {:?}",
            needle,
            decoded.strings
        );
    }
    // default_sample_type points at "alloc_space".
    let dst = decoded
        .default_sample_type
        .expect("default_sample_type missing");
    assert_eq!(
        decoded.strings[dst as usize], "alloc_space",
        "default_sample_type must point at \"alloc_space\""
    );
    // alloc_objects axis sums to sample count.
    assert_eq!(
        decoded.alloc_objects_total as usize,
        snap.len(),
        "alloc_objects axis must equal sample count"
    );

    cleanup();
}

/// Empty profile produces a valid Profile message.  Runs in both
/// feature configs because the OFF build also takes this path
/// (every snapshot is empty).
#[test]
fn write_pprof_empty_snapshot() {
    let p = HeapProfile::default();
    assert!(p.is_empty());

    let mut buf: Vec<u8> = Vec::new();
    p.write_pprof(&mut buf, Weight::Allocated)
        .expect("empty profile write is infallible");
    assert!(
        !buf.is_empty(),
        "even an empty Profile must contain the sample_type axes + string \
         table; got zero bytes"
    );

    let decoded = decode_profile(&buf);
    // No samples, no locations, no functions.
    assert_eq!(decoded.sample_count, 0);
    assert_eq!(decoded.location_count, 0);
    assert_eq!(decoded.function_count, 0);
    // But the sample-type metadata and default_sample_type hint
    // are always present.
    assert_eq!(decoded.sample_type_count, 2);
    assert!(decoded.default_sample_type.is_some());
    assert!(decoded.strings.iter().any(|s| s == "alloc_space"));
    assert!(decoded.strings.iter().any(|s| s == "alloc_objects"));
}

/// sum(sample.value[1]) over the encoded Profile must equal
/// HeapProfile::total_allocated_bytes under Weight::Allocated.  This
/// is the structural invariant that the bytes axis must preserve;
/// without it, any pprof-driven dashboard would display the wrong
/// totals.
#[test]
fn pprof_total_weight_matches_total_allocated_bytes() {
    let _lock = workload_lock();
    let a = SnMalloc::new();
    if !a.profiling_supported() {
        return;
    }

    let (snap, cleanup) = run_workload(50);

    let mut buf: Vec<u8> = Vec::new();
    snap.write_pprof(&mut buf, Weight::Allocated)
        .expect("Vec<u8> write is infallible");

    let decoded = decode_profile(&buf);
    assert_eq!(
        decoded.alloc_space_total as u128,
        snap.total_allocated_bytes(),
        "sum of alloc_space axis ({}) does not equal \
         total_allocated_bytes ({})",
        decoded.alloc_space_total,
        snap.total_allocated_bytes()
    );

    cleanup();
}
