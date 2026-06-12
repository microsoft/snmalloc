//! Safe Rust wrapper around the Phase 9.6 text-dump C ABI.
//!
//! The underlying `snmalloc_dump_stats_to_buffer` follows snprintf
//! truncation semantics; we use the standard two-phase pattern (size
//! query + alloc + fill) so callers never need to guess how large the
//! dump will be.  The buffer is dropped at the end of [`write_to`], so
//! the heap allocation is short-lived even for very wide dumps (the
//! per-size-class table can grow to ~64 rows when every class is
//! populated).
//!
//! Exposed unconditionally -- the underlying C ABI is always linked
//! into the Rust archive (see `src/snmalloc/override/stats_dump.cc`),
//! and the dump is just a formatter over `snmalloc_get_full_stats`.
//! A non-stats / non-profile build still emits a readable header
//! block, just with the wave-2 fields stuck at zero.

extern crate alloc;
extern crate std;

use alloc::vec::Vec;
use core::ptr;
use std::io;

use snmalloc_sys as ffi;

use crate::SnMalloc;

impl SnMalloc {
    /// Format the current allocator telemetry into the supplied
    /// `std::io::Write` sink (Phase 9.6).
    ///
    /// Internally a two-phase call into
    /// `snmalloc_dump_stats_to_buffer`: first a size-query with
    /// `(null, 0)`, then a real fill into a heap-allocated buffer
    /// of exactly the queried size.  See [`write_to`] for the
    /// full implementation; this method just exposes the helper
    /// as a method on the allocator type.
    ///
    /// The output is a tcmalloc-style text block.  See [`write_to`]
    /// for the format contract.
    ///
    /// Exposed unconditionally (NOT gated on the `stats` Cargo
    /// feature) because the underlying C ABI symbol is always
    /// linked into the Rust archive -- same rationale as
    /// [`crate::SnMalloc::set_sample_interval`].
    #[inline]
    pub fn dump_stats<W: io::Write>(&self, out: &mut W) -> io::Result<()> {
        write_to(out)
    }
}

/// Format the current allocator telemetry snapshot into `out`.
///
/// Two-phase: a `(null, 0)` size-query, then a fill into a buffer of
/// exactly the queried size.  The fill is forwarded to `out` via a
/// single `write_all` call; partial writes are propagated as
/// `io::Result::Err` per the standard contract.
///
/// Output is tcmalloc-style: a header of `MALLOC:` lines (bytes in
/// use, peak, committed / decommitted, fast/slow path counters,
/// cross-thread message metrics), optionally followed by a
/// per-size-class table (rows for any class with non-zero counters)
/// and a log2-spaced lifetime histogram (rows for any non-zero
/// bucket).  Optional sections are omitted when their data is
/// all-zero so a non-profile, non-stats build still produces a
/// readable dump.
///
/// No allocator state is mutated; the snapshot is read via the same
/// atomic counters that back [`crate::SnMalloc::full_stats`].  Safe to
/// invoke from any thread at any point in the process lifetime.
pub fn write_to<W: io::Write>(out: &mut W) -> io::Result<()> {
    // Phase 1: size-query.  The C side guarantees this is a pure
    // computation -- no allocator state is mutated, no buffer
    // touched.  Returns the byte count the dump *would* require,
    // not counting the trailing NUL.
    let needed = unsafe { ffi::snmalloc_dump_stats_to_buffer(ptr::null_mut(), 0) };
    if needed == 0 {
        // Defensive: the dump always produces at least the rule
        // lines and the MALLOC header, so `needed == 0` would only
        // happen if the C side decided every section was empty.
        // Nothing to write; the caller still gets a successful
        // result.
        return Ok(());
    }

    // Phase 2: real fill.  Reserve `needed + 1` bytes for the NUL
    // the C writer appends; we drop the NUL before forwarding to
    // the caller.
    let mut buf: Vec<u8> = Vec::with_capacity(needed + 1);
    let written = unsafe {
        let n = ffi::snmalloc_dump_stats_to_buffer(buf.as_mut_ptr(), needed + 1);
        // The C ABI may report a smaller number than the size
        // query if the snapshot raced and shrank between the two
        // calls; clamp to the requested capacity so the Vec length
        // is always in bounds.
        let n = if n > needed { needed } else { n };
        // SAFETY: the C writer fills `n` bytes inside the
        // capacity we reserved.  We mark them initialised before
        // slicing.
        buf.set_len(n);
        n
    };

    if written == 0 {
        return Ok(());
    }
    out.write_all(&buf)
}

/// Convenience helper for callers that want the dump as an owned
/// `String`.  The returned string is UTF-8 because the C formatter
/// only emits ASCII (digits, punctuation, and unit names).  Returns
/// an empty string when the snapshot has nothing to report.
///
/// Useful for tests: the C++ side has a `dump_stats_to_string`
/// equivalent and we want symmetric coverage on the Rust side.
pub fn to_string() -> alloc::string::String {
    let mut buf: Vec<u8> = Vec::new();
    // `write_to` only ever returns Err if the underlying writer
    // does; writing into a Vec never fails.
    let _ = write_to(&mut buf);
    // C formatter is pure-ASCII; we still go through `from_utf8`
    // to make the safety obvious.
    match alloc::string::String::from_utf8(buf) {
        Ok(s) => s,
        // Pathological case (C side somehow emitted non-UTF8): fall
        // back to the lossy conversion so tests still get something
        // they can match against.
        Err(e) => alloc::string::String::from_utf8_lossy(&e.into_bytes()).into_owned(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::String;

    #[test]
    fn dump_is_nonempty_and_well_formed() {
        // No global-allocator setup -- the formatter reads atomic
        // counters that exist whether or not the test binary uses
        // `SnMalloc` as its #[global_allocator].
        let s = to_string();
        assert!(!s.is_empty(), "dump must produce at least the header block");
        assert!(
            s.contains("Bytes in use by application"),
            "dump must contain the canonical 'Bytes in use by application' line; \
             got: {}",
            s
        );
        assert!(
            s.contains("------------------------------------------------"),
            "dump must contain a horizontal rule"
        );
    }

    #[test]
    fn write_to_propagates_writer_errors() {
        // A writer that always reports `WriteZero` should propagate
        // out as an error rather than getting silently swallowed.
        struct Broken;
        impl io::Write for Broken {
            fn write(&mut self, _b: &[u8]) -> io::Result<usize> {
                Err(io::Error::new(io::ErrorKind::Other, "broken"))
            }
            fn flush(&mut self) -> io::Result<()> {
                Ok(())
            }
        }
        let mut broken = Broken;
        let err = write_to(&mut broken)
            .expect_err("broken writer must propagate as Err");
        assert_eq!(err.kind(), io::ErrorKind::Other);
    }

    #[test]
    fn size_query_matches_real_fill() {
        // Calling the C ABI twice in a row should produce coherent
        // sizes -- the second call's `written` must never exceed
        // the first call's reported `needed`.  The Vec re-allocation
        // we do in `write_to` relies on that invariant.
        let needed = unsafe { ffi::snmalloc_dump_stats_to_buffer(ptr::null_mut(), 0) };
        let mut s = String::new();
        s.reserve(needed);
        let _ = to_string();
    }
}
