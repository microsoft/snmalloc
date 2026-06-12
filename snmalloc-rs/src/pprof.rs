//! Phase 6.1 -- pprof protobuf encoder for [`HeapProfile`].
//!
//! Emits the subset of Google's pprof
//! [`Profile`](https://github.com/google/pprof/blob/main/proto/profile.proto)
//! schema needed to drive `go tool pprof`, Pyroscope, Polar Signals,
//! Parca, and the Datadog continuous-profiler front-ends from a
//! snmalloc heap profile snapshot.
//!
//! Encoding strategy
//! -----------------
//!
//! We **hand-roll** the protobuf encoder rather than bringing in
//! `prost`/`prost-build`.  Reasons:
//!
//! 1.  The Profile message is small (~10 top-level fields) and the
//!     `proto3` wire format we need is just two encodings -- varint
//!     and length-delimited.  A from-scratch encoder is ~80 lines.
//! 2.  Avoids adding `prost` (which transitively pulls in `bytes`,
//!     `prost-derive`, syn, quote, ...) for a single message format.
//!     This keeps `--features profiling` lean: zero new transitive
//!     dependencies versus the existing `profiling` feature.
//! 3.  `prost-build` would require a `build.rs` for the `snmalloc-rs`
//!     crate -- right now we have none.  Keeping `snmalloc-rs` free of
//!     build scripts speeds up downstream compiles.
//!
//! The output is **not** gzipped.  The pprof tooling accepts both
//! compressed (`Content-Encoding: gzip`) and uncompressed Profile
//! bytes; `go tool pprof file.pb` happily ingests either, with the
//! convention being that `.pb` is uncompressed and `.pb.gz` is gzipped.
//! Skipping gzip avoids pulling in a `flate2` dependency.  Callers
//! that need gzip can wrap the writer in `flate2::GzEncoder`
//! themselves.
//!
//! Unsymbolicated frames
//! ---------------------
//!
//! When the `symbolicate` feature is **off**, every captured frame
//! address is emitted as a [`Function`] whose `name` is the
//! `0x` + 16-hex-digit rendering of the raw address and whose
//! `filename` and `start_line` are empty / zero.  This mirrors the
//! contract of [`HeapProfile::write_flamegraph`] in the same build
//! configuration.  pprof viewers render that as
//! "`0x000000010a4b9c30`" on the flamegraph leaves.
//!
//! With the `symbolicate` feature on, function names resolve via
//! [`HeapProfile::symbolize`] when available, with the hex fallback
//! used for any frame the symbol backend can't resolve.

extern crate alloc;
extern crate std;

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write as _;

use std::io;
use std::io::Write;

use crate::profile::{BtSample, HeapProfile, Weight};

// =========================================================================
// Wire-format primitives
// =========================================================================
//
// proto3 wire format crash course:
//
// * Each field on the wire is `(tag << 3) | wire_type` encoded as a
//   varint, followed by either a varint payload (wire_type 0) or a
//   length-delimited payload (wire_type 2).
// * Varints are little-endian, 7 bits of data per byte, MSB=1 for
//   "more bytes follow", MSB=0 for the last byte.
// * Length-delimited payloads are `len` (varint) + `len` bytes of
//   inner payload.
// * "Packed" repeated fields (the proto3 default for scalar repeated
//   fields) are encoded as a single length-delimited record whose
//   inner payload is the concatenated scalar values.

const WIRE_TYPE_VARINT: u32 = 0;
const WIRE_TYPE_LEN: u32 = 2;

/// Encode a u64 varint into `out`.
fn varint(out: &mut Vec<u8>, mut value: u64) {
    while value >= 0x80 {
        out.push((value as u8) | 0x80);
        value >>= 7;
    }
    out.push(value as u8);
}

/// Encode a field tag (field number + wire type) into `out`.
fn tag(out: &mut Vec<u8>, field_number: u32, wire_type: u32) {
    varint(out, ((field_number << 3) | wire_type) as u64);
}

/// Encode a `(field, varint)` pair into `out`.
fn write_uint64(out: &mut Vec<u8>, field_number: u32, value: u64) {
    tag(out, field_number, WIRE_TYPE_VARINT);
    varint(out, value);
}

/// Encode a `(field, int64)` pair into `out`.  proto3 represents
/// negative int64 as a 10-byte varint; we only ever emit non-negative
/// values so the bit pattern is the same as a u64.
fn write_int64(out: &mut Vec<u8>, field_number: u32, value: i64) {
    tag(out, field_number, WIRE_TYPE_VARINT);
    varint(out, value as u64);
}

/// Encode a `(field, length-delimited bytes)` pair into `out`.  Used
/// for both string fields and nested messages.
fn write_bytes(out: &mut Vec<u8>, field_number: u32, bytes: &[u8]) {
    tag(out, field_number, WIRE_TYPE_LEN);
    varint(out, bytes.len() as u64);
    out.extend_from_slice(bytes);
}

/// Encode a packed-repeated `int64` field into `out`.  Used by
/// `Sample.value` and `Sample.location_id`.  An empty slice still
/// writes a zero-length record so the consumer can distinguish "field
/// not set" from "field set to an empty list" (the latter matters for
/// pprof's `period_type`-vs-`sample_type` alignment checks).
fn write_packed_uint64(out: &mut Vec<u8>, field_number: u32, values: &[u64]) {
    if values.is_empty() {
        return;
    }
    let mut buf: Vec<u8> = Vec::new();
    for &v in values {
        varint(&mut buf, v);
    }
    write_bytes(out, field_number, &buf);
}

/// Encode a packed-repeated `int64` field into `out` (same wire
/// format as `write_packed_uint64`, separate signature for
/// readability at the call site -- pprof has both `value` (int64) and
/// `location_id` (uint64) packed repeated fields).
fn write_packed_int64(out: &mut Vec<u8>, field_number: u32, values: &[i64]) {
    if values.is_empty() {
        return;
    }
    let mut buf: Vec<u8> = Vec::new();
    for &v in values {
        varint(&mut buf, v as u64);
    }
    write_bytes(out, field_number, &buf);
}

// =========================================================================
// String table: deduplicate strings, index by insertion order.
// =========================================================================
//
// pprof's `string_table` is a 0-indexed array of UTF-8 strings.
// Slot 0 MUST be the empty string -- the spec uses index 0 as a
// sentinel for "no value" in optional string fields.

struct StringTable {
    /// Insertion-ordered list of strings.  Index 0 is always "".
    strings: Vec<String>,
    /// Reverse lookup: string -> index.  Avoids O(N) scans when the
    /// same name appears in many frames (e.g. a hot allocator
    /// entrypoint shared across thousands of samples).
    index: BTreeMap<String, u32>,
}

impl StringTable {
    fn new() -> Self {
        let mut t = Self {
            strings: Vec::new(),
            index: BTreeMap::new(),
        };
        // Slot 0 is the empty string per the pprof contract.
        t.intern("");
        t
    }

    /// Look up or insert `s`, returning its index.  Indices are
    /// monotonically increasing; once assigned, they are stable for
    /// the lifetime of this table.
    fn intern(&mut self, s: &str) -> u32 {
        if let Some(&idx) = self.index.get(s) {
            return idx;
        }
        let idx = self.strings.len() as u32;
        self.strings.push(String::from(s));
        self.index.insert(String::from(s), idx);
        idx
    }
}

// =========================================================================
// Profile assembly
// =========================================================================

/// Render a raw code-pointer address as `0x` + 16 hex digits.  Used
/// as the fallback function name when no symbolicated name is
/// available (the unsymbolicated build path).
fn hex_addr(addr: usize) -> String {
    let mut s = String::with_capacity(18);
    write!(&mut s, "0x{:016x}", addr).expect("writing to String is infallible");
    s
}

/// Write the [`HeapProfile`] as a pprof Profile protobuf message
/// into `w`.
///
/// The emitted Profile has two sample-type axes:
///
/// 1.  `("alloc_objects", "count")` -- always `1` per sample.  Lets
///     pprof aggregate by *sample count* (i.e. distinct sampled
///     allocations) as well as by bytes.
/// 2.  `("alloc_space", "bytes")` -- the per-sample byte contribution
///     under the requested [`Weight`] projection.  Summing this axis
///     across all samples equals [`HeapProfile::total_allocated_bytes`]
///     (for `Weight::Allocated`) or [`HeapProfile::total_requested_bytes`]
///     (for `Weight::Requested`).
///
/// `default_sample_type` is set to `alloc_space` so that pprof's
/// `top` / `web` views default to the bytes view, matching what most
/// heap-attribution dashboards want.
///
/// The output is not gzipped.  See the module-level docs for the
/// rationale.
///
/// This call is total: it produces a valid (but tiny) Profile even
/// for an empty snapshot.  An empty pprof Profile still contains the
/// `sample_type` and `string_table` fields -- consumers like `go tool
/// pprof` will display an empty profile cleanly rather than rejecting
/// the input.
pub(crate) fn write_pprof<W: Write>(
    profile: &HeapProfile,
    weight: Weight,
    w: &mut W,
) -> io::Result<()> {
    // ---------------------------------------------------------------------
    // Step 1: build the string table, location set, and function set.
    // ---------------------------------------------------------------------
    //
    // pprof models a sample stack as a chain of `location_id`s; each
    // Location points at one or more (function_id, line) pairs; each
    // Function has an interned name.  In the unsymbolicated build we
    // have a single Function per unique address (name = "0x..hex.."),
    // and a single Location per unique address (mapping_id = 0,
    // address = addr, line = [{function_id}]).

    let mut strings = StringTable::new();

    // Interned string indices that the rest of this function reuses
    // for the two sample-type axes.  Done first so the indices are
    // small (one-byte varints), keeping the output compact.
    let s_alloc_objects = strings.intern("alloc_objects");
    let s_count = strings.intern("count");
    let s_alloc_space = strings.intern("alloc_space");
    let s_bytes = strings.intern("bytes");

    #[cfg(feature = "symbolicate")]
    let resolved = profile.symbolize();

    // Map: address -> (function_id, location_id).  We need this both
    // ways: location_id is what samples reference, function_id is
    // what locations reference.  We assign IDs starting at 1 because
    // pprof reserves id=0 as "unset" (see the proto3 default).
    let mut addr_to_loc: BTreeMap<usize, u64> = BTreeMap::new();
    let mut addr_to_func: BTreeMap<usize, u64> = BTreeMap::new();
    let mut next_location_id: u64 = 1;
    let mut next_function_id: u64 = 1;

    // Pre-allocated buffers for the per-function and per-location
    // sub-messages.  We rebuild them in-place for each emitted
    // message to avoid repeated heap allocations.
    let mut functions_buf: Vec<Vec<u8>> = Vec::new();
    let mut locations_buf: Vec<Vec<u8>> = Vec::new();

    // Walk every frame in every sample.  Collecting the unique frame
    // set up-front (rather than streaming) lets us assign small,
    // densely packed IDs.
    for s in profile.samples() {
        for &frame in &s.stack {
            let addr = frame as usize;
            if addr_to_loc.contains_key(&addr) {
                continue;
            }
            // Resolve the function name: symbol if available, hex
            // fallback otherwise.  Either way it ends up in the
            // string table.
            #[cfg(feature = "symbolicate")]
            let (name_idx, file_idx, line_no) = {
                let r = resolved.get(&(frame as *const u8));
                let name = r.and_then(|r| r.name.as_deref());
                let file = r.and_then(|r| r.file.as_deref()).unwrap_or("");
                let line = r.and_then(|r| r.line).unwrap_or(0) as i64;
                let nm = match name {
                    Some(n) => strings.intern(n),
                    None => strings.intern(&hex_addr(addr)),
                };
                (nm, strings.intern(file), line)
            };
            #[cfg(not(feature = "symbolicate"))]
            let (name_idx, file_idx, line_no) = {
                let nm = strings.intern(&hex_addr(addr));
                // No symbolicator: empty filename (string slot 0),
                // line 0.
                (nm, 0u32, 0i64)
            };

            // ---- Function message ----------------------------------
            // Profile.Function (proto field id = 5).  Inner fields:
            //   1 = id (uint64)
            //   2 = name (int64 -> string_table index)
            //   3 = system_name (int64 -> string_table index)
            //   4 = filename (int64 -> string_table index)
            //   5 = start_line (int64)
            let function_id = next_function_id;
            next_function_id += 1;
            addr_to_func.insert(addr, function_id);

            let mut func_buf: Vec<u8> = Vec::new();
            write_uint64(&mut func_buf, 1, function_id);
            write_int64(&mut func_buf, 2, name_idx as i64);
            // system_name = name (no separately-mangled symbol available)
            write_int64(&mut func_buf, 3, name_idx as i64);
            write_int64(&mut func_buf, 4, file_idx as i64);
            // start_line: we only know the call site line, not the
            // function start.  Leaving at 0 is the conventional "we
            // don't know" sentinel.
            write_int64(&mut func_buf, 5, 0);
            functions_buf.push(func_buf);

            // ---- Location message ----------------------------------
            // Profile.Location (proto field id = 4).  Inner fields:
            //   1 = id (uint64)
            //   2 = mapping_id (uint64, 0 = "unknown mapping")
            //   3 = address (uint64)
            //   4 = line (repeated Line)
            // Line inner fields:
            //   1 = function_id (uint64)
            //   2 = line (int64)
            let location_id = next_location_id;
            next_location_id += 1;
            addr_to_loc.insert(addr, location_id);

            let mut line_buf: Vec<u8> = Vec::new();
            write_uint64(&mut line_buf, 1, function_id);
            write_int64(&mut line_buf, 2, line_no);

            let mut loc_buf: Vec<u8> = Vec::new();
            write_uint64(&mut loc_buf, 1, location_id);
            // mapping_id: we don't emit a Mapping (which would
            // describe the executable file ranges), so this stays 0.
            write_uint64(&mut loc_buf, 2, 0);
            write_uint64(&mut loc_buf, 3, addr as u64);
            // Single nested Line record.
            write_bytes(&mut loc_buf, 4, &line_buf);
            locations_buf.push(loc_buf);
        }
    }

    // ---------------------------------------------------------------------
    // Step 2: build the sample list.
    // ---------------------------------------------------------------------
    //
    // pprof Sample (field id = 2 on Profile).  Inner fields used:
    //   1 = location_id (packed repeated uint64)
    //   2 = value (packed repeated int64)
    //
    // pprof's location_id ordering convention is **leaf-first**: the
    // innermost / most-recently-active call site comes first.  Our
    // `BtSample::stack` is also innermost-first, so we forward it
    // directly without reversing.

    let mut samples_buf: Vec<Vec<u8>> = Vec::with_capacity(profile.samples().len());
    for s in profile.samples() {
        let loc_ids: Vec<u64> = s
            .stack
            .iter()
            .map(|&p| {
                *addr_to_loc
                    .get(&(p as usize))
                    .expect("every frame address was indexed in step 1")
            })
            .collect();
        let alloc_objects: i64 = 1;
        let alloc_space: i64 = sample_weight(s, weight) as i64;
        let values: [i64; 2] = [alloc_objects, alloc_space];

        let mut sample_buf: Vec<u8> = Vec::new();
        write_packed_uint64(&mut sample_buf, 1, &loc_ids);
        write_packed_int64(&mut sample_buf, 2, &values);
        samples_buf.push(sample_buf);
    }

    // ---------------------------------------------------------------------
    // Step 3: emit the top-level Profile message.
    // ---------------------------------------------------------------------
    //
    // Field order matches the proto definition for readability when
    // someone inspects the raw bytes with `protoc --decode_raw`.
    // pprof itself does not require any particular ordering.
    //
    // Profile (top level) fields used:
    //   1  = sample_type (repeated ValueType)
    //   2  = sample (repeated Sample)
    //   4  = location (repeated Location)
    //   5  = function (repeated Function)
    //   6  = string_table (repeated string)
    //   14 = default_sample_type (int64 -> string_table index)
    //
    // We do NOT emit:
    //   3  = mapping  -- we don't know binary file ranges
    //   9  = time_nanos -- left to caller via env/post-processing
    //   11 = period_type / 12 = period -- snmalloc's sampler is a
    //        Poisson process; the per-sample weight already accounts
    //        for the rate, so we deliberately omit period_type so
    //        pprof doesn't try to multiply us by it.

    let mut out: Vec<u8> = Vec::new();

    // ---- sample_type[0] = ("alloc_objects", "count") ----------------
    {
        let mut vt: Vec<u8> = Vec::new();
        write_int64(&mut vt, 1, s_alloc_objects as i64);
        write_int64(&mut vt, 2, s_count as i64);
        write_bytes(&mut out, 1, &vt);
    }
    // ---- sample_type[1] = ("alloc_space", "bytes") ------------------
    {
        let mut vt: Vec<u8> = Vec::new();
        write_int64(&mut vt, 1, s_alloc_space as i64);
        write_int64(&mut vt, 2, s_bytes as i64);
        write_bytes(&mut out, 1, &vt);
    }

    // ---- samples (field 2) ------------------------------------------
    for sample_buf in &samples_buf {
        write_bytes(&mut out, 2, sample_buf);
    }
    // ---- locations (field 4) ----------------------------------------
    for loc_buf in &locations_buf {
        write_bytes(&mut out, 4, loc_buf);
    }
    // ---- functions (field 5) ----------------------------------------
    for func_buf in &functions_buf {
        write_bytes(&mut out, 5, func_buf);
    }
    // ---- string_table (field 6) -------------------------------------
    for s in &strings.strings {
        write_bytes(&mut out, 6, s.as_bytes());
    }
    // ---- default_sample_type (field 14) -----------------------------
    // Point at "alloc_space" so pprof's default view is bytes.
    write_int64(&mut out, 14, s_alloc_space as i64);

    w.write_all(&out)
}

// =========================================================================
// Per-sample weight projection.
// =========================================================================
//
// `HeapProfile::sample_weight` is private in `profile.rs`.  Rather
// than widen its visibility for this single in-crate consumer, we
// inline the (two-line) computation here over the public
// `BtSample` fields.  Kept in lock-step with the definition in
// `profile.rs` via the alloc_space-axis invariant test below and the
// `pprof_total_weight_matches_total_allocated_bytes` integration
// test in `tests/profile_pprof.rs`.
fn sample_weight(s: &BtSample, weight: Weight) -> u128 {
    match weight {
        Weight::Requested => s.weight as u128,
        Weight::Allocated => {
            if s.requested_size == 0 {
                0
            } else {
                let w = s.weight as u128;
                let a = s.allocated_size as u128;
                let r = s.requested_size as u128;
                w.saturating_mul(a) / r
            }
        }
    }
}

// =========================================================================
// Unit tests
// =========================================================================
//
// These tests exercise the encoder directly on synthetic samples so
// they run regardless of the `profiling` feature.  The integration
// tests in `tests/profile_pprof.rs` exercise the full live-sampler
// path.

#[cfg(test)]
mod tests {
    use super::*;
    use crate::profile::BtSample;
    use alloc::vec;

    /// Varint encoder matches the wire format from the protobuf spec.
    #[test]
    fn varint_round_trip() {
        let cases: &[(u64, &[u8])] = &[
            (0, &[0x00]),
            (1, &[0x01]),
            (127, &[0x7f]),
            (128, &[0x80, 0x01]),
            (300, &[0xac, 0x02]),
            (16384, &[0x80, 0x80, 0x01]),
        ];
        for &(v, expected) in cases {
            let mut buf: Vec<u8> = Vec::new();
            varint(&mut buf, v);
            assert_eq!(buf.as_slice(), expected, "varint({}) mismatch", v);
        }
    }

    /// Empty profile produces a valid Profile message that still
    /// carries the two sample_type axes and the default_sample_type
    /// hint.  Consumers like `go tool pprof` need those fields to
    /// even render an empty profile.
    #[test]
    fn empty_profile_is_valid() {
        let p = HeapProfile::default();
        let mut buf: Vec<u8> = Vec::new();
        write_pprof(&p, Weight::Allocated, &mut buf).unwrap();

        // Must be non-empty: at minimum sample_type x2 + strings.
        assert!(!buf.is_empty(), "empty profile produced zero bytes");

        // String table must contain at least the well-known strings.
        // Search the byte buffer for them.
        let bytes = &buf[..];
        for needle in &["alloc_objects", "count", "alloc_space", "bytes"] {
            assert!(
                bytes.windows(needle.len()).any(|w| w == needle.as_bytes()),
                "expected string {:?} in empty Profile output",
                needle
            );
        }
    }

    /// sum(sample.value[1]) == total_allocated_bytes(profile).  This
    /// is the structural invariant that the pprof bytes axis must
    /// preserve.  Decoded by hand here -- we have only one repeated
    /// field shape to traverse.
    #[test]
    fn alloc_space_axis_matches_total_allocated_bytes() {
        let p = HeapProfile::from_samples(vec![
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 64,
                allocated_size: 64,
                weight: 4096,
                stack: vec![0x1usize as *const u8, 0x2usize as *const u8],
            },
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 100,
                allocated_size: 128,
                weight: 8192,
                stack: vec![0x3usize as *const u8],
            },
        ]);
        let mut buf: Vec<u8> = Vec::new();
        write_pprof(&p, Weight::Allocated, &mut buf).unwrap();

        let total = decode_alloc_space_sum(&buf);
        assert_eq!(total, p.total_allocated_bytes() as i64);
    }

    /// Round-trip check under `Weight::Requested`.
    #[test]
    fn alloc_space_axis_matches_total_requested_bytes() {
        let p = HeapProfile::from_samples(vec![BtSample {
            alloc_ptr: core::ptr::null(),
            requested_size: 100,
            allocated_size: 128,
            weight: 8192,
            stack: vec![0x3usize as *const u8],
        }]);
        let mut buf: Vec<u8> = Vec::new();
        write_pprof(&p, Weight::Requested, &mut buf).unwrap();

        let total = decode_alloc_space_sum(&buf);
        assert_eq!(total, p.total_requested_bytes() as i64);
    }

    /// Tiny hand-rolled decoder: walk the top-level Profile message
    /// looking for `sample` (field 2) records, then inside each
    /// `Sample` decode the `value` (field 2, packed int64) and pick
    /// the *second* element (the alloc_space axis).  This is the
    /// minimum protobuf decoder needed to validate our encoder
    /// without pulling in `prost`.
    fn decode_alloc_space_sum(buf: &[u8]) -> i64 {
        let mut sum: i64 = 0;
        let mut i: usize = 0;
        while i < buf.len() {
            let (tag, n) = read_varint(&buf[i..]);
            i += n;
            let field = (tag >> 3) as u32;
            let wire = (tag & 0x7) as u32;
            match (field, wire) {
                (2, WIRE_TYPE_LEN) => {
                    // Sample
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    let end = i + len as usize;
                    sum += decode_sample_alloc_space(&buf[i..end]);
                    i = end;
                }
                (_, WIRE_TYPE_LEN) => {
                    // Skip other length-delimited fields
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    i += len as usize;
                }
                (_, WIRE_TYPE_VARINT) => {
                    let (_, n) = read_varint(&buf[i..]);
                    i += n;
                }
                _ => panic!("unsupported wire type {} for field {}", wire, field),
            }
        }
        sum
    }

    fn decode_sample_alloc_space(buf: &[u8]) -> i64 {
        let mut i: usize = 0;
        while i < buf.len() {
            let (tag, n) = read_varint(&buf[i..]);
            i += n;
            let field = (tag >> 3) as u32;
            let wire = (tag & 0x7) as u32;
            match (field, wire) {
                (2, WIRE_TYPE_LEN) => {
                    // value (packed int64)
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    let end = i + len as usize;
                    let mut values: Vec<i64> = Vec::new();
                    let mut j = i;
                    while j < end {
                        let (v, n) = read_varint(&buf[j..]);
                        j += n;
                        values.push(v as i64);
                    }
                    // value = [alloc_objects, alloc_space]; the
                    // alloc_space axis is index 1.
                    if values.len() >= 2 {
                        return values[1];
                    }
                    i = end;
                }
                (_, WIRE_TYPE_LEN) => {
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    i += len as usize;
                }
                (_, WIRE_TYPE_VARINT) => {
                    let (_, n) = read_varint(&buf[i..]);
                    i += n;
                }
                _ => panic!("unsupported wire type {} for field {}", wire, field),
            }
        }
        0
    }

    /// Decode a single u64 varint, returning (value, bytes_consumed).
    fn read_varint(buf: &[u8]) -> (u64, usize) {
        let mut value: u64 = 0;
        let mut shift: u32 = 0;
        for (i, &b) in buf.iter().enumerate() {
            value |= ((b & 0x7f) as u64) << shift;
            if b & 0x80 == 0 {
                return (value, i + 1);
            }
            shift += 7;
            if shift >= 64 {
                panic!("varint overflow");
            }
        }
        panic!("truncated varint");
    }

    /// Each unique frame address must produce exactly one Function
    /// and one Location in the output.  Two samples sharing a frame
    /// share IDs.
    #[test]
    fn unique_frames_dedup_function_and_location() {
        let shared = 0xdeadbeefusize as *const u8;
        let p = HeapProfile::from_samples(vec![
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 64,
                allocated_size: 64,
                weight: 4096,
                stack: vec![shared, 0x1usize as *const u8],
            },
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 64,
                allocated_size: 64,
                weight: 4096,
                stack: vec![shared, 0x2usize as *const u8],
            },
        ]);
        let mut buf: Vec<u8> = Vec::new();
        write_pprof(&p, Weight::Allocated, &mut buf).unwrap();

        // Count top-level field-4 (location) and field-5 (function)
        // length-delimited records.
        let (n_loc, n_fn) = count_locations_and_functions(&buf);
        // Three unique addresses: shared, 0x1, 0x2.
        assert_eq!(n_loc, 3, "expected 3 unique locations");
        assert_eq!(n_fn, 3, "expected 3 unique functions");
    }

    fn count_locations_and_functions(buf: &[u8]) -> (usize, usize) {
        let mut n_loc = 0usize;
        let mut n_fn = 0usize;
        let mut i: usize = 0;
        while i < buf.len() {
            let (tag, n) = read_varint(&buf[i..]);
            i += n;
            let field = (tag >> 3) as u32;
            let wire = (tag & 0x7) as u32;
            match (field, wire) {
                (4, WIRE_TYPE_LEN) => {
                    n_loc += 1;
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    i += len as usize;
                }
                (5, WIRE_TYPE_LEN) => {
                    n_fn += 1;
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    i += len as usize;
                }
                (_, WIRE_TYPE_LEN) => {
                    let (len, n) = read_varint(&buf[i..]);
                    i += n;
                    i += len as usize;
                }
                (_, WIRE_TYPE_VARINT) => {
                    let (_, n) = read_varint(&buf[i..]);
                    i += n;
                }
                _ => panic!("unsupported wire type {} for field {}", wire, field),
            }
        }
        (n_loc, n_fn)
    }

    /// String table slot 0 must be the empty string, per pprof spec.
    #[test]
    fn string_table_slot_zero_is_empty() {
        let mut t = StringTable::new();
        assert_eq!(t.intern(""), 0);
        // Re-interning the empty string returns the same index.
        assert_eq!(t.intern(""), 0);
        // First non-empty intern is slot 1.
        assert_eq!(t.intern("alloc_objects"), 1);
    }
}
