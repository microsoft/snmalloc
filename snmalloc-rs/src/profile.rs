//! Safe Rust wrapper over the `sn_rust_profile_*` FFI surface added in
//! Phase 4.0.  This module is only compiled when the `profiling` Cargo
//! feature is enabled; the wrapper is itself purely a thin, owned data
//! type plus an RAII guard around the FFI snapshot handle.
//!
//! Memory model
//! ------------
//!
//! The C ABI in `rust.cc` exposes the snapshot as an opaque
//! `void*` handle.  Two failure modes need to be tolerated:
//!
//! 1.  Profiling is disabled at C-build time
//!     (`SNMALLOC_PROFILE` undefined).  `sn_rust_profile_supported()`
//!     returns `false`, `snapshot_begin` returns `NULL`, and the
//!     remaining FFI calls degrade to no-ops or `0`/`false` returns.
//!     This module mirrors that: [`HeapProfile`] is empty,
//!     [`SnMalloc::sampling_rate`] returns `0`,
//!     [`SnMalloc::set_sampling_rate`] is a no-op, and
//!     [`SnMalloc::profiling_supported`] returns `false`.
//!
//! 2.  Profiling is enabled but the snapshot allocation itself failed
//!     (out of memory inside the C bookkeeping).  `snapshot_begin`
//!     again returns `NULL`; we observe an empty snapshot, and the
//!     RAII guard tolerates the null handle on `Drop`.
//!
//! In both cases [`SnMalloc::snapshot`] is total: it never panics, and
//! it always releases any non-null FFI handle it acquires -- including
//! on panic mid-collection -- via an internal RAII guard whose `Drop`
//! impl calls `sn_rust_profile_snapshot_end`.

extern crate alloc;
extern crate std;

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write as _;

use std::io;

use snmalloc_sys as ffi;
use snmalloc_sys::SnRustProfileRawSample;

use crate::SnMalloc;

/// One sampled live allocation.
///
/// Field layout intentionally mirrors the raw C struct
/// `SnRustProfileRawSample` while normalising the C types into the
/// idiomatic Rust ones (`*const u8` instead of `*mut c_void`, `Vec`
/// instead of a fixed-length frame array).
///
/// `weight` is the byte-weight associated with this Poisson sample;
/// summing it across the snapshot gives an unbiased estimator of
/// total bytes requested by live allocations.  `allocated_size`
/// reflects the sizeclass-rounded bytes the allocator actually handed
/// back, while `requested_size` is what the caller asked for.
#[derive(Clone, Debug)]
pub struct BtSample {
    /// Pointer returned to the caller by the original allocation.
    /// Opaque -- intended only for debugging / cross-referencing
    /// with the application's own bookkeeping.  Stable inside a
    /// snapshot but not safe to dereference.
    pub alloc_ptr: *const u8,
    /// Number of bytes the original caller requested.
    pub requested_size: usize,
    /// Number of bytes actually returned (sizeclass-rounded).
    pub allocated_size: usize,
    /// Bytes-of-request weight for this Poisson sample.
    pub weight: usize,
    /// Captured return addresses, innermost first.  Symbolicating
    /// these into function names + line numbers is Phase 4.5; for
    /// now they are opaque code pointers.
    pub stack: Vec<*const u8>,
}

// SAFETY: BtSample contains raw pointers used purely as opaque
// integer-typed identifiers.  We never dereference them, and the
// snapshot is fully owned (Vec) -- so sending across threads or
// sharing is safe.
unsafe impl Send for BtSample {}
unsafe impl Sync for BtSample {}

/// Which per-sample weight projection to use when aggregating a
/// [`HeapProfile`] for export (e.g. a flame graph).
///
/// Both variants are unbiased Poisson estimators of byte counts; they
/// differ only in whether the per-sample "size" is the caller's
/// requested bytes or the allocator's sizeclass-rounded bytes:
///
/// - [`Weight::Allocated`] -- bytes the allocator actually returned,
///   i.e. `weight * allocated_size / requested_size`.  Matches the
///   "bytes mapped from snmalloc" view a heap-profile user usually
///   wants when chasing live-memory regressions, since it accounts
///   for sizeclass slack.  This is the default for
///   [`HeapProfile::write_flamegraph`].
/// - [`Weight::Requested`] -- bytes the caller asked for, i.e. just
///   the raw per-sample `weight`.  Matches the "bytes asked of malloc"
///   view, which is what most user-level heap-attribution dashboards
///   want.
///
/// See `docs/profile-weight.md` and Phase 4.3 of the heap-profiling
/// design for the rationale; in particular the default tracks the
/// `total_allocated_bytes` aggregator on [`HeapProfile`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Weight {
    /// Use the caller-requested byte count (raw per-sample weight).
    Requested,
    /// Use the allocator-returned byte count
    /// (weight * allocated_size / requested_size).
    Allocated,
}

impl Default for Weight {
    fn default() -> Self {
        Weight::Allocated
    }
}

/// An owned snapshot of currently-live sampled allocations.
///
/// Obtained from [`SnMalloc::snapshot`].  Holds no references into
/// the C-side profile state -- once construction returns, the C
/// snapshot handle is already released.
#[derive(Clone, Debug, Default)]
pub struct HeapProfile {
    samples: Vec<BtSample>,
}

impl HeapProfile {
    /// Internal constructor used by `SnMalloc::snapshot`.
    pub(crate) fn from_samples(samples: Vec<BtSample>) -> Self {
        Self { samples }
    }

    /// All sampled allocations captured by this snapshot.
    pub fn samples(&self) -> &[BtSample] {
        &self.samples
    }

    /// Number of samples in the snapshot.
    pub fn len(&self) -> usize {
        self.samples.len()
    }

    /// `true` iff the snapshot contains no samples.
    pub fn is_empty(&self) -> bool {
        self.samples.is_empty()
    }

    /// Unbiased estimator of total live bytes returned by the
    /// allocator, scaled per-sample by `allocated_size / requested_size`.
    ///
    /// Returned as `u128` so that aggregations over very large
    /// (multi-TiB) workloads cannot overflow on 64-bit targets.
    /// Samples whose `requested_size` is zero are skipped to avoid
    /// division-by-zero.
    pub fn total_allocated_bytes(&self) -> u128 {
        let mut total: u128 = 0;
        for s in &self.samples {
            if s.requested_size == 0 {
                continue;
            }
            let w = s.weight as u128;
            let a = s.allocated_size as u128;
            let r = s.requested_size as u128;
            total = total.saturating_add(w.saturating_mul(a) / r);
        }
        total
    }

    /// Unbiased estimator of total live bytes the application
    /// requested.  This is just the sum of per-sample weights.
    pub fn total_requested_bytes(&self) -> u128 {
        let mut total: u128 = 0;
        for s in &self.samples {
            total = total.saturating_add(s.weight as u128);
        }
        total
    }

    /// Per-sample byte contribution under the given [`Weight`]
    /// projection, as a `u128`.  Internal helper shared between
    /// [`HeapProfile::write_flamegraph_with`] and the
    /// `total_*_bytes` aggregators.  Samples with
    /// `requested_size == 0` contribute zero under
    /// [`Weight::Allocated`] -- mirroring [`Self::total_allocated_bytes`]
    /// -- and contribute their raw `weight` under
    /// [`Weight::Requested`].
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

    /// Write the profile in the **collapsed / folded-stack** format
    /// understood by Brendan Gregg's `flamegraph.pl`, Jon Gjengset's
    /// [`inferno-flamegraph`](https://github.com/jonhoo/inferno), and
    /// the [speedscope](https://www.speedscope.app/) viewer (via its
    /// "Brendan Gregg's collapsed stack format" importer).
    ///
    /// One line per *unique* stack:
    ///
    /// ```text
    /// 0x000000010a4b9c30;0x000000010a4b9b10;0x000000010a4b9a20 16384
    /// ```
    ///
    /// where:
    ///
    /// - frames are rendered as zero-padded 16-hex-digit code pointers,
    ///   ordered **root-first** (outermost on the left, innermost /
    ///   leaf on the right) as required by every collapsed-format
    ///   consumer; the in-memory [`BtSample::stack`] is innermost-first,
    ///   so we reverse on the way out, and
    /// - the trailing integer is the summed per-sample weight (in
    ///   bytes) across every snapshot sample whose stack is identical.
    ///
    /// The weight projection is [`Weight::Allocated`] -- bytes the
    /// allocator actually returned -- which matches the default UI
    /// view in `profile-weight.md`.  For [`Weight::Requested`] or
    /// other projections call [`HeapProfile::write_flamegraph_with`].
    ///
    /// Frames are rendered as raw hex code pointers; symbolicating
    /// them into function/file/line is Phase 4.5 (see
    /// [Symbolicator ticket]).  Consumers can pipe the output of this
    /// function directly into `flamegraph.pl` or `inferno-flamegraph`
    /// without any further processing:
    ///
    /// ```text
    /// my-binary > heap.folded     # your code calls write_flamegraph
    /// inferno-flamegraph < heap.folded > heap.svg
    /// ```
    ///
    /// This call is total: it is a no-op (writes zero bytes, returns
    /// `Ok(())`) on an empty profile -- including the
    /// profiling-feature-off build where every snapshot is empty.
    ///
    /// Performance: O(N) where N is the number of samples.  Internally
    /// a `BTreeMap` is used so that the output is deterministically
    /// ordered (stacks sorted lexicographically by their rendered
    /// hex-frame form) -- this matters for golden-output tests and
    /// for diffing two profiles in version control.
    ///
    /// Speedscope's native JSON schema is **not** emitted by this
    /// method; speedscope can import the folded format directly.  A
    /// dedicated `to_speedscope` is deferred to Phase 4.5+, where it
    /// can layer on top of the symbolicator and emit
    /// `frames`/`shared`/`profiles` records with real symbol names.
    pub fn write_flamegraph<W: io::Write>(&self, w: &mut W) -> io::Result<()> {
        self.write_flamegraph_with(Weight::Allocated, w)
    }

    /// Same as [`HeapProfile::write_flamegraph`], but with an explicit
    /// [`Weight`] projection.
    ///
    /// Stacks with zero total weight (e.g. every contributing sample
    /// had `requested_size == 0` under [`Weight::Allocated`]) are
    /// emitted with a trailing `0`; that mirrors the semantics of
    /// [`HeapProfile::total_allocated_bytes`] and avoids silently
    /// dropping samples whose call stacks would otherwise look like a
    /// loss of fidelity.
    pub fn write_flamegraph_with<W: io::Write>(
        &self,
        weight: Weight,
        w: &mut W,
    ) -> io::Result<()> {
        // Collapse samples with identical stacks by summing the chosen
        // weight projection.  Using `BTreeMap<String, u128>` keyed by
        // the pre-rendered (root-first, hex) form gives us:
        //   - O(1) lookup against the rendered key
        //   - deterministic output order (lex on the key)
        //   - no need for a custom Hash impl on Vec<*const u8>
        // The 18*N bytes spent on key strings (16 hex + leading 0x +
        // separator per frame) is negligible relative to the cost of
        // even a single OS-level memory mapping, and N here is the
        // unique-stack count, not the sample count.
        let mut folded: BTreeMap<String, u128> = BTreeMap::new();
        for s in &self.samples {
            let key = render_stack_key(&s.stack);
            let contribution = Self::sample_weight(s, weight);
            let entry = folded.entry(key).or_insert(0);
            *entry = entry.saturating_add(contribution);
        }

        for (stack, total) in &folded {
            // flamegraph.pl / inferno consume only ASCII; the stack
            // key is hex+';' (pure ASCII) and the weight is rendered
            // as a base-10 integer.  No locale, no formatting flags.
            writeln!(w, "{} {}", stack, total)?;
        }
        Ok(())
    }
}

/// Render one [`BtSample::stack`] as the root-first, `;`-joined
/// hex-frame key used in the collapsed format.
///
/// Empty stacks render as the empty string -- that yields a line
/// like ` 12345` (leading space) which both `flamegraph.pl` and
/// `inferno-flamegraph` tolerate, mapping the weight to an
/// unattributed "[unknown]" bar.  Skipping such samples would
/// silently lose weight from `total_*_bytes`, which is worse.
fn render_stack_key(stack: &[*const u8]) -> String {
    // Each frame renders as "0x" + 16 hex digits = 18 bytes, plus a
    // ';' separator between frames (no trailing ';').  Pre-size to
    // avoid repeated reallocations for deep stacks.
    let mut key = String::with_capacity(stack.len().saturating_mul(19));
    // BtSample::stack is innermost-first; the collapsed format wants
    // root-first.  Iterate in reverse.
    for (i, frame) in stack.iter().rev().enumerate() {
        if i > 0 {
            key.push(';');
        }
        // `write!` into a String is infallible (the underlying impl
        // never returns Err for fmt::Error), so unwrap is fine.
        // Zero-padded 16-hex matches the conventional 64-bit code
        // pointer width and gives stable, sortable keys.
        let addr = *frame as usize;
        write!(&mut key, "0x{:016x}", addr).expect("writing to String is infallible");
    }
    key
}

/// RAII wrapper around the C snapshot handle.
///
/// `snapshot_begin` allocates two `malloc`-owned blocks on the C side
/// (the handle struct and its samples array).  Both are released by
/// `snapshot_end`.  This guard guarantees that the release happens
/// even if the collection loop panics part-way through copying
/// samples -- in practice the only thing that can panic in that loop
/// is the `Vec::push` allocator running out of memory, but the
/// guarantee matters for correctness and for forward-compatibility
/// (e.g. if future code adds symbolicating allocators on top).
struct RawSnapshotGuard {
    handle: *mut core::ffi::c_void,
}

impl RawSnapshotGuard {
    /// Begin a new snapshot.  Always pairs with a `Drop`, even on a
    /// null handle (the underlying FFI tolerates null).
    fn begin() -> Self {
        let handle = unsafe { ffi::sn_rust_profile_snapshot_begin() };
        Self { handle }
    }

    /// Number of samples available in the snapshot.  Zero for a
    /// null handle.
    fn count(&self) -> usize {
        unsafe { ffi::sn_rust_profile_snapshot_count(self.handle) }
    }

    /// Copy one sample out of the snapshot.  Returns `None` when the
    /// underlying FFI reports failure (out of range, null handle,
    /// profiling disabled).
    fn get(&self, idx: usize) -> Option<SnRustProfileRawSample> {
        // Build a zero-initialised raw sample so we never observe
        // uninitialised stack frames if the C side returns true but
        // writes fewer than the full array (it does not today, but
        // the contract is "up to SN_RUST_PROFILE_STACK_FRAMES").
        let mut out = SnRustProfileRawSample {
            alloc_ptr: core::ptr::null_mut(),
            requested_size: 0,
            allocated_size: 0,
            weight: 0,
            stack_depth: 0,
            stack: [core::ptr::null_mut(); ffi::SN_RUST_PROFILE_STACK_FRAMES],
        };
        let ok = unsafe {
            ffi::sn_rust_profile_snapshot_get(self.handle, idx, &mut out)
        };
        if ok {
            Some(out)
        } else {
            None
        }
    }
}

impl Drop for RawSnapshotGuard {
    fn drop(&mut self) {
        // Safe: snapshot_end tolerates a null handle.  Idempotent
        // because we never call it twice (Drop runs at most once).
        unsafe { ffi::sn_rust_profile_snapshot_end(self.handle) };
    }
}

impl SnMalloc {
    /// Capture an owned snapshot of currently-live sampled allocations.
    ///
    /// Returns an empty [`HeapProfile`] when profiling is disabled at
    /// C-build time (`SNMALLOC_PROFILE` undefined) or when the
    /// snapshot allocation failed on the C side.
    ///
    /// The snapshot is materialised eagerly into owned `Vec`s; once
    /// this function returns, the underlying FFI handle is already
    /// freed.  The collection loop is panic-safe: an RAII guard
    /// releases the C handle on unwind.
    pub fn snapshot(&self) -> HeapProfile {
        if !self.profiling_supported() {
            return HeapProfile::default();
        }

        let guard = RawSnapshotGuard::begin();
        let count = guard.count();
        let mut samples: Vec<BtSample> = Vec::with_capacity(count);

        for idx in 0..count {
            let Some(raw) = guard.get(idx) else {
                // The snapshot is a static array on the C side; a
                // None here would mean the count and the contents
                // disagree -- shouldn't happen in practice but is
                // not worth panicking over.  Skip and continue.
                continue;
            };
            // Clamp the depth to the inline array bound to avoid an
            // out-of-bounds slice if the C side ever returns a
            // larger value.  `SN_RUST_PROFILE_STACK_FRAMES` is the
            // contractual upper bound.
            let depth = (raw.stack_depth as usize)
                .min(ffi::SN_RUST_PROFILE_STACK_FRAMES);
            let mut stack: Vec<*const u8> = Vec::with_capacity(depth);
            for i in 0..depth {
                stack.push(raw.stack[i] as *const u8);
            }
            samples.push(BtSample {
                alloc_ptr: raw.alloc_ptr as *const u8,
                requested_size: raw.requested_size,
                allocated_size: raw.allocated_size,
                weight: raw.weight,
                stack,
            });
        }

        // `guard` drops here, releasing the FFI handle.
        HeapProfile::from_samples(samples)
    }

    /// Set the mean sampling interval, in bytes.  Zero disables
    /// sampling.  No-op when profiling is not supported by the
    /// linked C++ build.
    pub fn set_sampling_rate(&self, bytes: usize) {
        unsafe { ffi::sn_rust_profile_set_sampling_rate(bytes) }
    }

    /// Get the current mean sampling interval, in bytes.  Returns
    /// `0` when profiling is not supported by the linked C++ build.
    pub fn sampling_rate(&self) -> usize {
        unsafe { ffi::sn_rust_profile_get_sampling_rate() }
    }

    /// Returns `true` iff the linked C++ build was compiled with
    /// `SNMALLOC_PROFILE=ON`.  When `false`, [`SnMalloc::snapshot`]
    /// always returns an empty profile and the sampling rate is
    /// fixed at zero.
    pub fn profiling_supported(&self) -> bool {
        unsafe { ffi::sn_rust_profile_supported() }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    /// `profiling_supported()` mirrors the underlying C build's
    /// `sn_rust_profile_supported()`.  Both branches of the feature
    /// gate are checked: with the Cargo `profiling` feature on the
    /// C side is built with `SNMALLOC_PROFILE=ON` (see
    /// `snmalloc-sys/build.rs`); with it off the C stubs return
    /// `false`.
    #[test]
    fn profiling_supported_matches_feature() {
        let a = SnMalloc::new();
        if cfg!(feature = "profiling") {
            assert!(
                a.profiling_supported(),
                "profiling feature on must imply SNMALLOC_PROFILE=ON on the C side"
            );
        } else {
            assert!(
                !a.profiling_supported(),
                "profiling feature off must imply SNMALLOC_PROFILE undefined; \
                 got profiling_supported() == true"
            );
        }
    }

    /// The sampling rate round-trips through the FFI getter/setter
    /// when the feature is on.  When it is off, the getter is fixed
    /// at zero and the setter is a no-op.  Restoring the original
    /// value at the end is important because the per-process sampler
    /// state is global and other tests in the same binary observe
    /// it.
    #[test]
    fn sampling_rate_round_trip() {
        let a = SnMalloc::new();
        let saved = a.sampling_rate();
        a.set_sampling_rate(8192);
        if cfg!(feature = "profiling") {
            assert_eq!(a.sampling_rate(), 8192);
        } else {
            assert_eq!(a.sampling_rate(), 0);
        }
        a.set_sampling_rate(saved);
        assert_eq!(a.sampling_rate(), saved);
    }

    /// A snapshot is always safe to take, even with no sampling
    /// activity in this process.  We don't assert on the sample
    /// count -- other tests, or the default Rust allocator wiring,
    /// may or may not have produced samples by the time this runs.
    #[test]
    fn snapshot_is_callable() {
        let a = SnMalloc::new();
        let snap = a.snapshot();
        let _ = snap.len();
        let _ = snap.is_empty();
        let _ = snap.total_allocated_bytes();
        let _ = snap.total_requested_bytes();
    }

    /// Empty profile has the expected accessor behaviour.
    #[test]
    fn empty_profile_accessors() {
        let p = HeapProfile::default();
        assert_eq!(p.len(), 0);
        assert!(p.is_empty());
        assert_eq!(p.total_allocated_bytes(), 0u128);
        assert_eq!(p.total_requested_bytes(), 0u128);
        assert!(p.samples().is_empty());
    }

    /// `total_*_bytes` aggregate correctly across synthetic samples.
    /// Built from `from_samples` so this exercises the wrapper math
    /// independently of any live sampler activity.
    #[test]
    fn totals_are_computed() {
        let s = vec![
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 64,
                allocated_size: 64,
                weight: 4096,
                stack: vec![],
            },
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 100,
                allocated_size: 128,
                weight: 4096,
                stack: vec![],
            },
        ];
        let p = HeapProfile::from_samples(s);
        // requested-bytes estimator = sum(weight)
        assert_eq!(p.total_requested_bytes(), 4096u128 + 4096u128);
        // allocated-bytes estimator = sum(weight * allocated / requested)
        //                           = 4096 * 64/64 + 4096 * 128/100
        //                           = 4096 + 5242
        let expected = 4096u128 + 4096u128 * 128u128 / 100u128;
        assert_eq!(p.total_allocated_bytes(), expected);
    }

    /// Sample with `requested_size == 0` must be skipped instead of
    /// causing a divide-by-zero panic.
    #[test]
    fn zero_requested_size_skipped() {
        let s = vec![BtSample {
            alloc_ptr: core::ptr::null(),
            requested_size: 0,
            allocated_size: 0,
            weight: 12345,
            stack: vec![],
        }];
        let p = HeapProfile::from_samples(s);
        assert_eq!(p.total_allocated_bytes(), 0u128);
        // weight still contributes to the requested-bytes total --
        // that's the unbiased estimator regardless of any per-sample
        // size readings.
        assert_eq!(p.total_requested_bytes(), 12345u128);
    }

    /// `render_stack_key` reverses the innermost-first stack into
    /// root-first order, joins with `;`, and renders each frame as a
    /// zero-padded 16-hex code pointer.  Single-frame and empty
    /// stacks have their own contracts (see comments inline).
    #[test]
    fn stack_key_is_root_first_and_hex() {
        // Innermost-first sample stack: [leaf, mid, root].  The
        // emitted key must be root-first.
        let stack: Vec<*const u8> = vec![
            0x0badc0deusize as *const u8,
            0xdeadbeefusize as *const u8,
            0xfeedfaceusize as *const u8,
        ];
        let key = render_stack_key(&stack);
        assert_eq!(
            key,
            "0x00000000feedface;0x00000000deadbeef;0x000000000badc0de"
        );

        // Empty stack -> empty key (still safe to emit; consumers
        // render it as an "[unknown]" bar).
        assert_eq!(render_stack_key(&[]), "");

        // Single frame: no trailing/leading separator.
        let one: Vec<*const u8> = vec![0x42usize as *const u8];
        assert_eq!(render_stack_key(&one), "0x0000000000000042");
    }

    /// `write_flamegraph` on an empty profile writes nothing (zero
    /// bytes) and reports success.  This is the contract that lets
    /// the function be called unconditionally on the profiling-feature-off
    /// build, where every snapshot is empty.
    #[test]
    fn flamegraph_empty_profile_is_noop() {
        let p = HeapProfile::default();
        let mut out: std::vec::Vec<u8> = std::vec::Vec::new();
        p.write_flamegraph(&mut out).expect("infallible Vec<u8> write");
        assert!(out.is_empty());
    }

    /// Two samples with identical stacks must collapse into a single
    /// folded line whose weight is the sum.  The default projection
    /// is `Weight::Allocated`; with allocated == requested the per-
    /// sample contribution is just `weight`.
    #[test]
    fn flamegraph_collapses_identical_stacks() {
        let stack: Vec<*const u8> = vec![
            0xaaaausize as *const u8,
            0xbbbbusize as *const u8,
        ];
        let p = HeapProfile::from_samples(vec![
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 64,
                allocated_size: 64,
                weight: 4096,
                stack: stack.clone(),
            },
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 64,
                allocated_size: 64,
                weight: 4096,
                stack,
            },
        ]);
        let mut out: std::vec::Vec<u8> = std::vec::Vec::new();
        p.write_flamegraph(&mut out).unwrap();
        let s = std::string::String::from_utf8(out).unwrap();
        // Exactly one line, summed weight 8192.
        let lines: std::vec::Vec<&str> = s.lines().collect();
        assert_eq!(lines.len(), 1);
        assert_eq!(
            lines[0],
            "0x000000000000bbbb;0x000000000000aaaa 8192"
        );
    }

    /// Distinct stacks remain on separate lines and the total weight
    /// reported across the folded output matches
    /// `total_allocated_bytes` (the default projection).
    #[test]
    fn flamegraph_weight_sum_matches_total_allocated() {
        let p = HeapProfile::from_samples(vec![
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 64,
                allocated_size: 64,
                weight: 4096,
                stack: vec![0x1usize as *const u8],
            },
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 100,
                allocated_size: 128,
                weight: 4096,
                stack: vec![0x2usize as *const u8],
            },
        ]);
        let mut out: std::vec::Vec<u8> = std::vec::Vec::new();
        p.write_flamegraph(&mut out).unwrap();
        let s = std::string::String::from_utf8(out).unwrap();
        let lines: std::vec::Vec<&str> = s.lines().collect();
        assert_eq!(lines.len(), 2);

        let mut sum: u128 = 0;
        for line in lines {
            // Format: "<stack> <weight>".  Split on the rightmost
            // space; rsplitn protects against accidental spaces in a
            // stack rendering (there shouldn't be any -- everything
            // is hex+';' -- but the parser side is more robust this
            // way).
            let mut it = line.rsplitn(2, ' ');
            let w: u128 = it.next().unwrap().parse().unwrap();
            let _stack = it.next().unwrap();
            sum += w;
        }
        assert_eq!(sum, p.total_allocated_bytes());
    }

    /// Explicit `Weight::Requested` projection sums the raw weights
    /// (matching `total_requested_bytes`), independent of the
    /// allocated/requested ratio.
    #[test]
    fn flamegraph_requested_projection_matches_total_requested() {
        let p = HeapProfile::from_samples(vec![
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 64,
                allocated_size: 128,
                weight: 4096,
                stack: vec![0x1usize as *const u8],
            },
            BtSample {
                alloc_ptr: core::ptr::null(),
                requested_size: 100,
                allocated_size: 128,
                weight: 8192,
                stack: vec![0x2usize as *const u8],
            },
        ]);
        let mut out: std::vec::Vec<u8> = std::vec::Vec::new();
        p.write_flamegraph_with(Weight::Requested, &mut out).unwrap();
        let s = std::string::String::from_utf8(out).unwrap();
        let mut sum: u128 = 0;
        for line in s.lines() {
            let mut it = line.rsplitn(2, ' ');
            let w: u128 = it.next().unwrap().parse().unwrap();
            let _stack = it.next().unwrap();
            sum += w;
        }
        assert_eq!(sum, p.total_requested_bytes());
        assert_eq!(sum, 4096u128 + 8192u128);
    }

    /// `Weight::default()` is `Allocated` -- the default UI view per
    /// `profile-weight.md`.
    #[test]
    fn weight_default_is_allocated() {
        assert_eq!(Weight::default(), Weight::Allocated);
    }
}
