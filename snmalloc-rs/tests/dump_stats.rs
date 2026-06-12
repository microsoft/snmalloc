//! Integration test for the Phase 9.6 text-dump API.
//!
//! Exercises `SnMalloc::dump_stats(&mut impl Write)` end-to-end: the
//! Rust safe wrapper -> `snmalloc_dump_stats_to_buffer` C ABI ->
//! `snmalloc_get_full_stats` snapshot -> formatted output.  The
//! checks are structural: we assert that the dump contains the
//! canonical tcmalloc-style header lines without pinning the exact
//! integer values (which depend on whatever other tests cargo runs
//! in parallel against the same process-global counters).
//!
//! This test lives in its own integration-test binary (separate from
//! the other `tests/*.rs` files) for the same reason `full_stats.rs`
//! does -- the underlying counters are process-global, and an
//! isolated binary gives us a deterministic measurement window
//! independent of what other tests are doing.

use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};

/// The dump always contains a canonical "MALLOC: ... Bytes in use by
/// application" line per the tcmalloc heritage.  We pin that string
/// rather than the numeric prefix because the integers depend on
/// process state at the moment of the call.
fn assert_canonical_header(dump: &str) {
    assert!(
        dump.contains("Bytes in use by application"),
        "dump must contain the canonical 'Bytes in use by application' \
         line; got:\n{}",
        dump
    );
    // The header block uses horizontal rules of 48 dashes.
    assert!(
        dump.contains("------------------------------------------------"),
        "dump must contain at least one horizontal rule; got:\n{}",
        dump
    );
    // All header lines start with `MALLOC:`.
    assert!(
        dump.contains("MALLOC:"),
        "dump must contain at least one MALLOC: line; got:\n{}",
        dump
    );
}

#[test]
fn dump_stats_emits_canonical_header() {
    let alloc = SnMalloc::new();
    let mut buf: Vec<u8> = Vec::new();
    alloc
        .dump_stats(&mut buf)
        .expect("writing to a Vec never fails");

    assert!(!buf.is_empty(), "dump_stats produced no output");
    let dump = std::str::from_utf8(&buf)
        .expect("dump must be ASCII / UTF-8");
    assert_canonical_header(dump);
}

#[test]
fn dump_stats_reflects_live_allocation() {
    // After driving real traffic through the allocator, the dump
    // must still emit a coherent block.  We don't assert that
    // bytes_in_use jumped (the dump is text, not numbers; we want
    // structural correctness here).  The dedicated `full_stats.rs`
    // covers the underlying numeric invariants.
    let alloc = SnMalloc::new();
    let layout = Layout::from_size_align(1 << 20, 64).unwrap();
    let ptr = unsafe { alloc.alloc(layout) };
    assert!(!ptr.is_null(), "1 MiB allocation must not fail");

    let mut buf: Vec<u8> = Vec::new();
    alloc
        .dump_stats(&mut buf)
        .expect("writing to a Vec never fails");
    let dump = std::str::from_utf8(&buf).expect("dump must be UTF-8");
    assert_canonical_header(dump);

    // Free first so a panic in the assert below still releases the
    // allocation (Vec / dump have already been computed).
    unsafe { alloc.dealloc(ptr, layout) };

    // Sanity: the dump must mention "Peak bytes in use" (this is the
    // line that explicitly carries the high-water-mark, which we
    // know is non-zero given we just allocated 1 MiB).
    assert!(
        dump.contains("Peak bytes in use"),
        "dump must contain the 'Peak bytes in use' line; got:\n{}",
        dump
    );
}

#[test]
fn dump_stats_two_calls_are_independent() {
    // Two back-to-back calls into `dump_stats` must each return a
    // self-contained, header-bearing block -- there should be no
    // hidden state that makes the second call shorter than the first.
    let alloc = SnMalloc::new();

    let mut a: Vec<u8> = Vec::new();
    let mut b: Vec<u8> = Vec::new();
    alloc.dump_stats(&mut a).unwrap();
    alloc.dump_stats(&mut b).unwrap();

    assert_canonical_header(std::str::from_utf8(&a).unwrap());
    assert_canonical_header(std::str::from_utf8(&b).unwrap());

    // The two dumps should be of roughly similar length (they may
    // not be byte-identical if other tests happened to change the
    // counters between calls, but neither should be empty).
    assert!(!a.is_empty());
    assert!(!b.is_empty());
}

#[test]
fn dump_stats_regex_match() {
    // Lightweight golden structural check.  Instead of pulling in
    // the `regex` crate (which would bloat the dev-dependency
    // surface), we substring-match the canonical line shape:
    //   "MALLOC:" + whitespace + integer + whitespace + "(<num> <unit>)"
    //   + whitespace + "Bytes in use by application"
    let alloc = SnMalloc::new();
    let mut buf: Vec<u8> = Vec::new();
    alloc.dump_stats(&mut buf).unwrap();
    let dump = std::str::from_utf8(&buf).unwrap();

    // Find the bytes-in-use line and tear off its prefix; the
    // prefix must start with "MALLOC:" and contain a digit and an
    // open-paren for the human-readable column.
    let line = dump
        .lines()
        .find(|l| l.contains("Bytes in use by application"))
        .expect("dump must contain a 'Bytes in use by application' line");
    assert!(line.starts_with("MALLOC:"), "line must start with MALLOC:; got {:?}", line);
    assert!(line.contains('('), "line must contain a human-readable parenthesized column; got {:?}", line);
    assert!(line.contains(')'), "line must contain a closing paren; got {:?}", line);
    assert!(
        line.chars().any(|c| c.is_ascii_digit()),
        "line must contain at least one digit; got {:?}",
        line
    );
}
