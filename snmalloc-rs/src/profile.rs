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

#[cfg(feature = "symbolicate")]
use std::collections::HashMap;

/// Event kind tag attached to a [`BtSample`].
///
/// Snapshot samples are always [`SampleKind::Alloc`]: the persisted
/// per-object slot is never re-tagged on resize -- only the streaming
/// broadcast carries a `Resize` event.  The enum is exposed here so
/// snapshot consumers can pattern-match symmetrically with streaming
/// consumers (where the same idea is exposed as
/// [`crate::streaming::EventKind`]); the variants are also forward-
/// compatible with future kinds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SampleKind {
    /// A fresh sampled allocation.  This is the only kind produced by
    /// `SnMalloc::snapshot` in the current implementation.
    Alloc,
    /// An in-place realloc updated an existing sample's size.  Not
    /// currently emitted by snapshot mode -- reserved so that future
    /// snapshot consumers can match exhaustively against a single enum
    /// shared with the streaming surface.
    Resize,
}

impl SampleKind {
    /// Decode the raw `kind` byte from a [`SnRustProfileRawSample`].
    /// Unknown values fall back to [`SampleKind::Alloc`].
    #[inline]
    fn from_raw(kind: u8) -> Self {
        match kind {
            snmalloc_sys::SN_RUST_PROFILE_KIND_RESIZE => SampleKind::Resize,
            _ => SampleKind::Alloc,
        }
    }
}

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

impl BtSample {
    /// Event kind accessor, for symmetry with the streaming-mode
    /// [`crate::streaming::StreamSample::kind`] API.  Snapshot mode
    /// always returns [`SampleKind::Alloc`]: the persisted SampledList
    /// slot never carries a `Resize` tag -- only the streaming
    /// broadcast does (ticket 86aj0hk9y).  Exposing the accessor here
    /// regardless lets snapshot- and streaming-mode consumers share
    /// the same `kind()` shape.
    #[inline]
    pub fn kind(&self) -> SampleKind {
        SampleKind::Alloc
    }
}

// SAFETY: BtSample contains raw pointers used purely as opaque
// integer-typed identifiers.  We never dereference them, and the
// snapshot is fully owned (Vec) -- so sending across threads or
// sharing is safe.
unsafe impl Send for BtSample {}
unsafe impl Sync for BtSample {}

/// Grouping key for [`HeapProfile::top_sites`].
///
/// Each variant collapses samples that share the chosen key into a
/// single hot-spot row whose `inclusive_bytes` is the sum of the
/// per-sample [`Weight::Allocated`] projection.  See the method
/// docs on [`HeapProfile::top_sites`] for the full semantics.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HotSpotKey {
    /// Group by the deepest non-allocator frame.  In the
    /// unsymbolicated build this currently degrades to
    /// [`HotSpotKey::LeafFrame`] (we cannot tell allocator frames
    /// from user frames by address alone); the `symbolicate`
    /// feature gives this variant its full intended meaning in a
    /// follow-up.
    CallSite,
    /// Group by the innermost (deepest) frame in each sample's
    /// captured stack.  Most precise "which exact return address
    /// allocated" view.
    LeafFrame,
    /// Group by the entire captured stack as an ordered sequence.
    /// Two samples land in the same row iff every frame matches.
    FullStack,
}

/// One row in the [`HeapProfile::top_sites`] result.
///
/// All bytes are reported under the [`Weight::Allocated`]
/// projection.  `inclusive_bytes` is `u128` for the same overflow-
/// safety reason as [`HeapProfile::total_allocated_bytes`].
#[derive(Clone, Debug)]
pub struct HotSite {
    /// Innermost frame of the originating stack(s).  For
    /// [`HotSpotKey::FullStack`] grouping this is `stack[0]`; for
    /// [`HotSpotKey::CallSite`] / [`HotSpotKey::LeafFrame`] this
    /// is the single frame that was used as the bucket key.
    /// Address `0` denotes "no stack captured" (an unusual case
    /// produced only by sampler-internal failures to walk the
    /// stack).
    pub leaf_frame: *const u8,
    /// The frames that make up the key.  For
    /// [`HotSpotKey::CallSite`] / [`HotSpotKey::LeafFrame`] this
    /// holds a single element (the leaf); for
    /// [`HotSpotKey::FullStack`] it holds the full captured stack
    /// in innermost-first order, matching [`BtSample::stack`].
    pub stack: Vec<*const u8>,
    /// Sum of the [`Weight::Allocated`] projection across every
    /// sample that bucketed under this row's key.
    pub inclusive_bytes: u128,
    /// Number of distinct snapshot samples that bucketed here.
    pub sample_count: u64,
}

// SAFETY: HotSite carries raw pointers used purely as opaque
// integer-typed identifiers (frame return addresses).  We never
// dereference them; the rest of the struct is owned data.
unsafe impl Send for HotSite {}
unsafe impl Sync for HotSite {}

/// Captured frames returned by [`crate::SnMalloc::lookup_alloc_site`].
///
/// `frames` is innermost-first to match [`BtSample::stack`].
/// `base_addr` and `allocated_size` describe the live byte range
/// the original lookup address fell into -- callers can derive the
/// offset of the queried interior pointer as `addr - base_addr`.
#[derive(Clone, Debug)]
pub struct Frames {
    /// Captured return addresses, innermost first.
    pub frames: Vec<*const u8>,
    /// Base address of the matched live allocation.
    pub base_addr: *const u8,
    /// Sizeclass-rounded byte length of the matched live allocation.
    pub allocated_size: usize,
}

// SAFETY: Frames carries raw pointers used purely as opaque
// integer-typed identifiers (frame return addresses and a base
// allocation pointer).  We never dereference them; the rest of the
// struct is owned data.
unsafe impl Send for Frames {}
unsafe impl Sync for Frames {}

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
///
/// # Example
///
/// ```no_run
/// # #[cfg(feature = "profiling")]
/// # fn main() -> std::io::Result<()> {
/// use snmalloc_rs::{SnMalloc, Weight};
///
/// let allocator = SnMalloc::new();
/// let profile = allocator.snapshot();
///
/// // Bytes the allocator actually returned (sizeclass-rounded).
/// let allocated = profile.total_allocated_bytes();
/// // Bytes the caller requested.
/// let requested = profile.total_requested_bytes();
///
/// // Render a flamegraph weighted by what the caller asked for.
/// let mut out: Vec<u8> = Vec::new();
/// profile.write_flamegraph_with(Weight::Requested, &mut out)?;
///
/// assert_eq!(Weight::default(), Weight::Allocated);
/// let _ = (allocated, requested);
/// # Ok(())
/// # }
/// # #[cfg(not(feature = "profiling"))]
/// # fn main() {}
/// ```
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

/// One symbolicated stack frame: a raw code pointer paired with the
/// best-effort function name, source file, and line number resolved
/// from the host process's debug information.
///
/// All three text fields are `Option<...>` because the backtrace
/// crate's `resolve_frame_unsynchronized` callback may legitimately
/// report nothing for a frame (kernel/JIT/no-debug-info code, stripped
/// binaries, ASLR-only loaded shared libraries, etc.).  Callers that
/// want a graceful fallback to hex should pair this with the
/// raw [`BtSample::stack`] -- [`HeapProfile::write_flamegraph_symbolized`]
/// does so by emitting `0x..` when `name.is_none()`.
///
/// Only present when the `symbolicate` Cargo feature is enabled.  See
/// [`HeapProfile::symbolize`].
#[cfg(feature = "symbolicate")]
#[derive(Clone, Debug, Default)]
pub struct ResolvedFrame {
    /// The raw code-pointer key this frame was resolved from.  Stable
    /// inside one process lifetime and matches the values in
    /// [`BtSample::stack`].
    pub address: *const u8,
    /// Demangled function name, e.g.
    /// `snmalloc_rs::profile::HeapProfile::snapshot`.
    /// `None` when the address falls in code without symbol info.
    pub name: Option<String>,
    /// Source file path, when known.
    pub file: Option<String>,
    /// 1-based source line, when known.
    pub line: Option<u32>,
}

// SAFETY: ResolvedFrame carries a raw `*const u8` as an opaque
// integer-typed identifier (never dereferenced).  The owned String
// fields are themselves Send + Sync; the pointer is treated as a
// value, not a reference, so it's safe to send the struct between
// threads.
#[cfg(feature = "symbolicate")]
unsafe impl Send for ResolvedFrame {}
#[cfg(feature = "symbolicate")]
unsafe impl Sync for ResolvedFrame {}

/// An owned snapshot of currently-live sampled allocations.
///
/// Obtained from [`SnMalloc::snapshot`].  Holds no references into
/// the C-side profile state -- once construction returns, the C
/// snapshot handle is already released.
///
/// # Example
///
/// Capture a snapshot and iterate the samples:
///
/// ```no_run
/// # #[cfg(feature = "profiling")]
/// # fn main() {
/// use snmalloc_rs::SnMalloc;
///
/// let allocator = SnMalloc::new();
/// // Enable Poisson sampling at ~256 KiB intervals.
/// allocator.set_sampling_rate(262_144);
///
/// // ... run the workload you want to profile ...
///
/// let profile = allocator.snapshot();
/// for sample in profile.samples() {
///     println!(
///         "alloc {:p}: requested {} bytes, returned {} bytes, weight {}, depth {}",
///         sample.alloc_ptr,
///         sample.requested_size,
///         sample.allocated_size,
///         sample.weight,
///         sample.stack.len(),
///     );
/// }
/// # }
/// # #[cfg(not(feature = "profiling"))]
/// # fn main() {}
/// ```
#[derive(Clone, Debug, Default)]
pub struct HeapProfile {
    samples: Vec<BtSample>,
}

impl HeapProfile {
    /// Construct a [`HeapProfile`] from an owned vector of samples.
    ///
    /// Primarily used by [`SnMalloc::snapshot`] to publish the
    /// snapshot collected through the FFI, but also exposed
    /// publicly so test code and downstream consumers can build a
    /// synthetic profile from `BtSample` values (e.g. to exercise
    /// the [`HeapProfile::top_sites`] aggregator or to replay a
    /// pre-recorded profile).
    pub fn from_samples(samples: Vec<BtSample>) -> Self {
        Self { samples }
    }

    /// All sampled allocations captured by this snapshot.
    ///
    /// # Example
    ///
    /// ```no_run
    /// # #[cfg(feature = "profiling")]
    /// # fn main() {
    /// use snmalloc_rs::SnMalloc;
    ///
    /// let allocator = SnMalloc::new();
    /// let profile = allocator.snapshot();
    ///
    /// // Bucket the sampled live allocations by their sizeclass-rounded size.
    /// let mut by_size: std::collections::BTreeMap<usize, usize> =
    ///     std::collections::BTreeMap::new();
    /// for s in profile.samples() {
    ///     *by_size.entry(s.allocated_size).or_insert(0) += 1;
    /// }
    /// for (size, count) in &by_size {
    ///     println!("{} bytes: {} samples", size, count);
    /// }
    /// # }
    /// # #[cfg(not(feature = "profiling"))]
    /// # fn main() {}
    /// ```
    pub fn samples(&self) -> &[BtSample] {
        &self.samples
    }

    /// Number of samples in the snapshot.
    pub fn len(&self) -> usize {
        self.samples.len()
    }

    /// Log2-spaced allocation-lifetime histogram (Phase 9.5).
    ///
    /// Returns a snapshot of the process-wide histogram of sampled
    /// allocation lifetimes, in nanoseconds.  Bucket `i` covers
    /// lifetimes whose `floor(log2(lifetime_ns))` equals `i`; bucket
    /// 31 saturates for lifetimes >= 2^31 ns (~2.1 s).  The buckets
    /// accumulate across the entire process lifetime -- not just this
    /// `HeapProfile` -- so two successive calls let consumers compute
    /// a delta over a measurement window.
    ///
    /// When the underlying snmalloc build was compiled without
    /// `SNMALLOC_PROFILE` (i.e. [`SnMalloc::profiling_supported`]
    /// returns `false`) the histogram is necessarily all zeros: no
    /// sample ever fires, so no lifetime is recorded.
    pub fn lifetime_histogram() -> [u64; ffi::SN_RUST_PROFILE_LIFETIME_BUCKETS] {
        let mut buckets = [0u64; ffi::SN_RUST_PROFILE_LIFETIME_BUCKETS];
        // SAFETY: passing a stack-local `[u64; N]` and its length; the
        // FFI implementation writes at most `len` `u64`s and treats the
        // buffer as opaque.  On unsupported builds the call writes
        // nothing and returns 0.
        let _written = unsafe {
            ffi::sn_rust_profile_lifetime_histogram(
                buckets.as_mut_ptr(),
                ffi::SN_RUST_PROFILE_LIFETIME_BUCKETS,
            )
        };
        buckets
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
    ///
    /// # Example
    ///
    /// ```no_run
    /// # #[cfg(feature = "profiling")]
    /// # fn main() {
    /// use snmalloc_rs::SnMalloc;
    ///
    /// let allocator = SnMalloc::new();
    /// let profile = allocator.snapshot();
    ///
    /// // Compare the two estimators: requested vs sizeclass-rounded.
    /// let allocated = profile.total_allocated_bytes();
    /// let requested = profile.total_requested_bytes();
    /// println!("live allocated ~{} B, live requested ~{} B", allocated, requested);
    /// # }
    /// # #[cfg(not(feature = "profiling"))]
    /// # fn main() {}
    /// ```
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

    /// Return the top `n` hot-spots in this profile, ranked by
    /// inclusive allocated bytes under the given [`HotSpotKey`]
    /// grouping.  Pure post-processing over the existing snapshot
    /// samples; no FFI calls.
    ///
    /// "Inclusive" here means: every sample whose stack matches the
    /// grouping key contributes its full [`Weight::Allocated`]
    /// projection to the bucket.  Two samples whose stacks differ in
    /// some non-key frame will still aggregate into the same row when
    /// they share the key frame(s) -- which is exactly the semantic
    /// callers want when investigating "where is all the memory being
    /// allocated by call site X".
    ///
    /// The three available groupings:
    ///
    /// - [`HotSpotKey::CallSite`] -- group by the deepest (innermost)
    ///   frame in each stack that is *not* one of the allocator's own
    ///   internal frames.  In the unsymbolicated build we cannot tell
    ///   allocator frames apart from user frames by name, so this
    ///   degrades to "the deepest (innermost) frame in each stack"
    ///   -- functionally equivalent to [`HotSpotKey::LeafFrame`].
    ///   When the `symbolicate` feature is on, future revisions of
    ///   this method may skip frames whose resolved name begins with
    ///   an allocator prefix (e.g. `snmalloc_rs::`, `__rust_alloc`).
    /// - [`HotSpotKey::LeafFrame`] -- group by the innermost frame
    ///   (`stack[0]`).  Most precise "which exact instruction
    ///   pointer allocated" view; samples with an empty stack land
    ///   in a single "<unknown>" bucket keyed on the null pointer.
    /// - [`HotSpotKey::FullStack`] -- group by the entire captured
    ///   stack as an ordered sequence.  Differs from `LeafFrame`
    ///   exactly when two different *callers* of the same leaf
    ///   function would otherwise collapse into one row.
    ///
    /// Output is sorted by descending inclusive bytes; ties broken
    /// by descending sample count, then ascending key (for
    /// determinism).  Returns at most `n` entries; `n = 0` returns
    /// an empty vec.
    ///
    /// # Example
    ///
    /// ```no_run
    /// # #[cfg(feature = "profiling")]
    /// # fn main() {
    /// use snmalloc_rs::{SnMalloc, HotSpotKey};
    ///
    /// let allocator = SnMalloc::new();
    /// let profile = allocator.snapshot();
    ///
    /// for site in profile.top_sites(10, HotSpotKey::LeafFrame) {
    ///     println!(
    ///         "leaf {:p}: {} samples, ~{} live bytes",
    ///         site.leaf_frame,
    ///         site.sample_count,
    ///         site.inclusive_bytes,
    ///     );
    /// }
    /// # }
    /// # #[cfg(not(feature = "profiling"))]
    /// # fn main() {}
    /// ```
    pub fn top_sites(&self, n: usize, key: HotSpotKey) -> Vec<HotSite> {
        if n == 0 {
            return Vec::new();
        }

        // Group key: a vec of frame addresses representing the
        // canonical key shape.  CallSite/LeafFrame produce single-
        // element keys (innermost frame); FullStack produces the
        // whole stack.  Using Vec<*const u8> uniformly avoids the
        // overhead of an enum-keyed map while still letting us
        // reconstruct the leaf for the HotSite output.
        //
        // `BTreeMap` keeps the bucketing deterministic and lets us
        // break ties by ascending key without an extra sort step.
        let mut buckets: BTreeMap<Vec<usize>, (u128, u64)> = BTreeMap::new();
        for s in &self.samples {
            let group_key: Vec<usize> = match key {
                HotSpotKey::CallSite | HotSpotKey::LeafFrame => {
                    // Innermost (leaf) frame, or 0 if empty.  Using
                    // usize for the key keeps Ord well-defined
                    // (raw pointers don't implement Ord in core).
                    let leaf = s
                        .stack
                        .first()
                        .copied()
                        .map(|p| p as usize)
                        .unwrap_or(0);
                    alloc::vec![leaf]
                }
                HotSpotKey::FullStack => {
                    s.stack.iter().map(|p| *p as usize).collect()
                }
            };
            let contribution = Self::sample_weight(s, Weight::Allocated);
            let entry = buckets.entry(group_key).or_insert((0u128, 0u64));
            entry.0 = entry.0.saturating_add(contribution);
            entry.1 = entry.1.saturating_add(1);
        }

        // Flatten to a Vec so we can sort by descending bytes.
        let mut rows: Vec<HotSite> = buckets
            .into_iter()
            .map(|(k, (bytes, count))| {
                // For Leaf/CallSite the single key entry *is* the
                // leaf.  For FullStack we still report the leaf
                // (the innermost frame) so the output shape is the
                // same across grouping modes.
                let leaf = k.first().copied().unwrap_or(0) as *const u8;
                let stack: Vec<*const u8> = match key {
                    HotSpotKey::FullStack => {
                        k.iter().map(|&u| u as *const u8).collect()
                    }
                    HotSpotKey::CallSite | HotSpotKey::LeafFrame => {
                        alloc::vec![leaf]
                    }
                };
                HotSite {
                    leaf_frame: leaf,
                    stack,
                    inclusive_bytes: bytes,
                    sample_count: count,
                }
            })
            .collect();

        // Descending bytes, then descending sample count, then
        // ascending leaf frame address (for determinism).
        rows.sort_by(|a, b| {
            b.inclusive_bytes
                .cmp(&a.inclusive_bytes)
                .then_with(|| b.sample_count.cmp(&a.sample_count))
                .then_with(|| (a.leaf_frame as usize).cmp(&(b.leaf_frame as usize)))
        });
        rows.truncate(n);
        rows
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
    ///
    /// # Example
    ///
    /// Capture a snapshot and write the folded-stack output to a file:
    ///
    /// ```no_run
    /// # #[cfg(feature = "profiling")]
    /// # fn main() -> std::io::Result<()> {
    /// use snmalloc_rs::SnMalloc;
    /// use std::fs::File;
    ///
    /// let allocator = SnMalloc::new();
    /// let profile = allocator.snapshot();
    ///
    /// let mut f = File::create("heap.folded")?;
    /// profile.write_flamegraph(&mut f)?;
    /// // Render with: `inferno-flamegraph < heap.folded > heap.svg`
    /// # Ok(())
    /// # }
    /// # #[cfg(not(feature = "profiling"))]
    /// # fn main() {}
    /// ```
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

    /// Write the profile in Google's [`pprof`][pprof] Profile
    /// protobuf format (Phase 6.1).
    ///
    /// Output is a raw (uncompressed) protobuf byte stream consumable
    /// by `go tool pprof`, [Pyroscope](https://pyroscope.io/),
    /// [Polar Signals Cloud](https://www.polarsignals.com/),
    /// [Parca](https://www.parca.dev/), and the Datadog continuous
    /// profiler.  Two sample-type axes are emitted:
    ///
    /// - `("alloc_objects", "count")` -- one count per sampled
    ///   allocation.
    /// - `("alloc_space", "bytes")` -- per-sample bytes under the
    ///   given [`Weight`] projection.  The default of
    ///   [`Weight::Allocated`] matches the rest of the snmalloc
    ///   profile surface; sum of this axis equals
    ///   [`HeapProfile::total_allocated_bytes`].
    ///
    /// Without the `symbolicate` Cargo feature, frame functions are
    /// named by their hex code-pointer (`"0x000000010a4b9c30"`) and
    /// the `filename` / `line` fields are empty -- mirroring the
    /// unsymbolicated path of [`HeapProfile::write_flamegraph`].
    /// With `symbolicate` on, function names, source files, and line
    /// numbers from [`HeapProfile::symbolize`] are emitted where
    /// available, with the hex fallback used for any unresolved
    /// frame.
    ///
    /// The output is **not gzipped**.  The pprof tooling accepts
    /// both encodings (`.pb` for uncompressed, `.pb.gz` for gzipped);
    /// for the gzipped form -- which is what Pyroscope, Polar Signals
    /// Cloud, Speedscope, and most cloud pprof importers expect on
    /// the wire -- use [`HeapProfile::write_pprof_gz`].  See
    /// `src/pprof.rs` for the encoder-design rationale.
    ///
    /// This call is total: it emits a valid (but tiny) Profile even
    /// on an empty snapshot -- including the profiling-feature-off
    /// build, where every snapshot is empty by construction.  An
    /// empty pprof Profile still carries the two `sample_type` axes
    /// and the `default_sample_type` hint so consumers render it
    /// cleanly rather than rejecting it.
    ///
    /// [pprof]: https://github.com/google/pprof/blob/main/proto/profile.proto
    ///
    /// # Example
    ///
    /// Render a snapshot into an in-memory pprof Profile and (optionally)
    /// persist it to a `.pb` file that `go tool pprof` can consume:
    ///
    /// ```no_run
    /// # #[cfg(feature = "profiling")]
    /// # fn main() -> std::io::Result<()> {
    /// use snmalloc_rs::{SnMalloc, Weight};
    ///
    /// let allocator = SnMalloc::new();
    /// let profile = allocator.snapshot();
    ///
    /// // Encode into a Vec<u8>; the encoder never grows past a
    /// // constant-factor of the input snapshot, so even very large
    /// // profiles fit comfortably in memory.
    /// let mut bytes: Vec<u8> = Vec::new();
    /// profile.write_pprof(&mut bytes, Weight::Allocated)?;
    ///
    /// // Optionally persist for `go tool pprof heap.pb`.
    /// std::fs::write("heap.pb", &bytes)?;
    /// # Ok(())
    /// # }
    /// # #[cfg(not(feature = "profiling"))]
    /// # fn main() {}
    /// ```
    pub fn write_pprof<W: io::Write>(&self, w: &mut W, weight: Weight) -> io::Result<()> {
        crate::pprof::write_pprof(self, weight, w)
    }

    /// Write the profile as a **gzip-wrapped** pprof Profile -- the
    /// `.pb.gz` encoding accepted natively by
    /// [Pyroscope](https://pyroscope.io/),
    /// [Polar Signals Cloud](https://www.polarsignals.com/),
    /// [Parca](https://www.parca.dev/),
    /// [Speedscope](https://www.speedscope.app/), and the Datadog
    /// continuous profiler as well as `go tool pprof`.
    ///
    /// Semantically equivalent to feeding the byte stream produced by
    /// [`HeapProfile::write_pprof`] through `flate2::write::GzEncoder`:
    /// the decoded payload is identical to the uncompressed pprof
    /// output, including the two `sample_type` axes, the
    /// `default_sample_type` hint, and the per-sample weight chosen by
    /// the [`Weight`] argument.  Round-tripping
    /// `write_pprof_gz(w, weight)` through `flate2::read::GzDecoder`
    /// yields exactly the same bytes as `write_pprof(w, weight)`.
    ///
    /// This call is total: it emits a valid (small) gzip stream even
    /// on an empty snapshot, matching the contract of
    /// [`HeapProfile::write_pprof`].  The first two output bytes are
    /// always the gzip magic `0x1f 0x8b`, so callers can content-sniff
    /// without parsing.
    ///
    /// Only available with the `profiling` Cargo feature, which
    /// transitively pulls in the `flate2` crate.  The rationale for
    /// gating gzip on the same feature as the rest of the profiler --
    /// rather than a dedicated `pprof-gz` -- is that gzipped pprof is
    /// the dominant on-the-wire encoding for every supported consumer,
    /// so adding a separate feature would multiply the build matrix
    /// without a meaningful payoff.
    ///
    /// # Example
    ///
    /// Render a snapshot directly into a `.pb.gz` file ready to upload
    /// to a continuous-profiler ingest endpoint:
    ///
    /// ```no_run
    /// # #[cfg(feature = "profiling")]
    /// # fn main() -> std::io::Result<()> {
    /// use snmalloc_rs::{SnMalloc, Weight};
    /// use std::fs::File;
    ///
    /// let allocator = SnMalloc::new();
    /// let profile = allocator.snapshot();
    ///
    /// let mut f = File::create("heap.pb.gz")?;
    /// profile.write_pprof_gz(&mut f, Weight::Allocated)?;
    /// # Ok(())
    /// # }
    /// # #[cfg(not(feature = "profiling"))]
    /// # fn main() {}
    /// ```
    #[cfg(feature = "profiling")]
    pub fn write_pprof_gz<W: io::Write>(
        &self,
        w: &mut W,
        weight: Weight,
    ) -> io::Result<()> {
        // Wrap the caller's writer in a GzEncoder, hand it to the
        // uncompressed encoder, then `finish()` to flush the gzip
        // trailer (without which `flate2::read::GzDecoder` and `gunzip`
        // both reject the stream with "unexpected end of file").
        // `Compression::default()` is level 6 -- the same default
        // `gzip(1)` uses; if benchmarks ever show this is a bottleneck
        // we can revisit, but for typical pprof sizes (tens to
        // hundreds of KiB) the difference between level 1 and level 6
        // is negligible compared to the encode-side protobuf work.
        let mut encoder = flate2::write::GzEncoder::new(
            w,
            flate2::Compression::default(),
        );
        self.write_pprof(&mut encoder, weight)?;
        // `finish()` writes the gzip footer + CRC.  Without this the
        // output is a truncated gzip stream -- silently accepted by
        // `Drop` (which calls `try_finish` and swallows errors) but
        // rejected by every conformant decoder.
        encoder.finish()?;
        Ok(())
    }

    /// Resolve every unique frame address in this profile to
    /// best-effort function/file/line metadata.
    ///
    /// The returned [`HashMap`] is keyed by the raw `*const u8`
    /// addresses that appear in [`BtSample::stack`], so callers can
    /// look up a frame in O(1) when rendering their own flamegraph or
    /// speedscope export.  Frames that the symbol backend cannot
    /// resolve still appear in the map -- with `name`, `file`, and
    /// `line` all `None` -- so the keyset is exactly the set of unique
    /// frame addresses in the profile.
    ///
    /// This is a deliberately heavyweight operation: under the hood it
    /// walks the host process's loaded debug info via the `backtrace`
    /// crate, which on macOS / Linux / Windows means parsing DWARF or
    /// PDB sections for every frame.  Call it once per snapshot, not
    /// per render.
    ///
    /// Only available with the `symbolicate` Cargo feature; that
    /// feature transitively pulls in the `backtrace` crate.  The
    /// design rationale -- pay the dependency cost only when callers
    /// opt in -- is documented in `Cargo.toml`.
    ///
    /// The output is a `HashMap`, not a `BTreeMap`, because callers
    /// typically use it as a lookup table from raw frame addresses
    /// (which are not meaningfully orderable) rather than iterating
    /// in a sorted order.
    #[cfg(feature = "symbolicate")]
    pub fn symbolize(&self) -> HashMap<*const u8, ResolvedFrame> {
        // Collect the set of unique frame addresses across the whole
        // snapshot first.  A typical workload has thousands of samples
        // but only hundreds of unique frames, and the backtrace
        // resolver is the slow part -- visiting each address exactly
        // once keeps `symbolize` roughly O(unique-frames), not
        // O(samples * stack-depth).
        let mut out: HashMap<*const u8, ResolvedFrame> = HashMap::new();
        for s in &self.samples {
            for &addr in &s.stack {
                // `entry(...).or_insert_with(...)` would also work,
                // but we want to avoid resolving the same address
                // twice, including in the (rare) case where the
                // address appears twice in the *same* stack (recursive
                // call site).  A two-step contains/insert dance keeps
                // the per-address resolve at one call.
                if out.contains_key(&addr) {
                    continue;
                }
                out.insert(addr, resolve_one(addr));
            }
        }
        out
    }

    /// Same as [`HeapProfile::write_flamegraph`], but emits resolved
    /// frame names (when available) instead of raw hex code pointers.
    ///
    /// For each frame:
    ///
    /// - if the symbolicator returned a non-`None` `name`, that name
    ///   is emitted verbatim.  Source-file and line information is
    ///   intentionally **not** appended -- the folded format is
    ///   ambiguous if frame strings contain spaces or `;` characters,
    ///   and most flamegraph viewers truncate the function name to
    ///   the part before the first space anyway.  Callers who want
    ///   richer metadata should call [`HeapProfile::symbolize`]
    ///   directly and render via a format that supports it (e.g.
    ///   speedscope JSON).
    /// - otherwise the frame falls back to the same
    ///   `0x` + 16-hex-digits rendering as [`HeapProfile::write_flamegraph`].
    ///
    /// Frame names are sanitised: any `;` or space character in a
    /// resolved name is replaced with `_`, since both characters are
    /// reserved separators in the folded format.  Without this, a
    /// resolved name containing `";"` would split a single frame into
    /// two on the consumer side.
    ///
    /// The output is sorted lexicographically by the rendered stack
    /// key, the same way [`HeapProfile::write_flamegraph`] sorts.
    /// Two samples with identical *resolved* stacks (which may differ
    /// in raw address -- e.g. inlining can produce distinct addresses
    /// that resolve to the same function) collapse to one folded
    /// line, with their weights summed.  The total weight emitted is
    /// therefore identical to [`HeapProfile::write_flamegraph`]'s
    /// total under the [`Weight::Allocated`] projection.
    ///
    /// Only available with the `symbolicate` Cargo feature.
    #[cfg(feature = "symbolicate")]
    pub fn write_flamegraph_symbolized<W: io::Write>(
        &self,
        w: &mut W,
    ) -> io::Result<()> {
        let resolved = self.symbolize();
        let mut folded: BTreeMap<String, u128> = BTreeMap::new();
        for s in &self.samples {
            let key = render_stack_key_symbolized(&s.stack, &resolved);
            let contribution = Self::sample_weight(s, Weight::Allocated);
            let entry = folded.entry(key).or_insert(0);
            *entry = entry.saturating_add(contribution);
        }
        for (stack, total) in &folded {
            writeln!(w, "{} {}", stack, total)?;
        }
        Ok(())
    }
}

/// Resolve a single frame address via the `backtrace` crate.  Returns
/// a [`ResolvedFrame`] with whatever metadata the symbol backend
/// supplied; absent fields stay `None`.
///
/// Some frames yield more than one [`backtrace::Symbol`] (typically
/// inlined functions).  We prefer the first symbol with a non-empty
/// name -- the outermost / "physical" function -- because that's the
/// one whose address actually matches the frame.  Inlined-function
/// details are useful for higher-fidelity tooling (speedscope JSON,
/// pprof) but would inflate a folded-stack line into something
/// ambiguous to the consumer.
#[cfg(feature = "symbolicate")]
fn resolve_one(addr: *const u8) -> ResolvedFrame {
    let mut frame = ResolvedFrame {
        address: addr,
        name: None,
        file: None,
        line: None,
    };
    // SAFETY: `resolve_unsynchronized` documents that it is unsafe
    // because it touches process-global symbolicator state without an
    // internal lock.  In practice our callers (`symbolize`) are
    // already single-threaded over their own `HeapProfile`, and the
    // backtrace crate's documented contract is satisfied for typical
    // application-level use.  We use the synchronised entry point
    // (`resolve`) instead so we don't need to enforce that contract
    // ourselves.
    backtrace::resolve(addr as *mut core::ffi::c_void, |sym| {
        // Only the first non-empty name wins; later inlined-frame
        // symbols are discarded (see function-level comment).
        if frame.name.is_none() {
            if let Some(name) = sym.name() {
                let demangled = alloc::format!("{}", name);
                if !demangled.is_empty() {
                    frame.name = Some(demangled);
                }
            }
        }
        if frame.file.is_none() {
            if let Some(path) = sym.filename() {
                if let Some(s) = path.to_str() {
                    frame.file = Some(String::from(s));
                }
            }
        }
        if frame.line.is_none() {
            if let Some(line) = sym.lineno() {
                frame.line = Some(line);
            }
        }
    });
    frame
}

/// Render a [`BtSample::stack`] as the root-first, `;`-joined key
/// used in the folded format -- with resolved frame names substituted
/// in wherever the symbolicator produced a non-`None` name.
///
/// Frames with no resolved name fall back to the same `0x` +
/// 16-hex-digit rendering used by [`render_stack_key`], so the
/// output is always non-empty for a non-empty stack.
///
/// Frame names are sanitised to keep the folded format
/// unambiguous: any `;` or space in a resolved name is replaced with
/// `_`.  Real-world Rust symbol names don't contain either character,
/// but symbols from `extern "C"` libraries or hand-crafted assembly
/// occasionally do, and a stray `;` would silently corrupt a single
/// frame into two on the consumer side.
#[cfg(feature = "symbolicate")]
fn render_stack_key_symbolized(
    stack: &[*const u8],
    resolved: &HashMap<*const u8, ResolvedFrame>,
) -> String {
    // Same pre-sizing rationale as render_stack_key: ~19 bytes per
    // hex frame plus a separator.  Symbolicated frames are wider on
    // average, but pre-sizing for the hex floor still cuts the number
    // of reallocations.
    let mut key = String::with_capacity(stack.len().saturating_mul(19));
    for (i, frame) in stack.iter().rev().enumerate() {
        if i > 0 {
            key.push(';');
        }
        let resolved_name = resolved
            .get(frame)
            .and_then(|r| r.name.as_deref());
        match resolved_name {
            Some(name) => {
                for ch in name.chars() {
                    // Reserved separators of the folded format.
                    if ch == ';' || ch == ' ' {
                        key.push('_');
                    } else {
                        key.push(ch);
                    }
                }
            }
            None => {
                let addr = *frame as usize;
                write!(&mut key, "0x{:016x}", addr)
                    .expect("writing to String is infallible");
            }
        }
    }
    key
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
            kind: snmalloc_sys::SN_RUST_PROFILE_KIND_ALLOC,
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
            // The C `kind` byte is currently `Alloc` for every persisted
            // sample (resize events live only in the streaming
            // broadcast).  Decode it for forward compatibility but do
            // not store it on `BtSample`: the public field set is
            // unchanged in v2 of the wire format.
            let _ = SampleKind::from_raw(raw.kind);
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

    /// Reverse-lookup the alloc-site of `addr` against the live
    /// sampled-allocation list.
    ///
    /// Returns the captured alloc-time call stack and the matched
    /// allocation's base / size iff:
    ///
    /// - the underlying allocation was selected by the Poisson sampler,
    /// - the allocation is still live at the moment of the call, and
    /// - `addr` falls inside `[base, base + allocated_size)` (interior
    ///   pointers are accepted).
    ///
    /// Returns `None` otherwise -- including for any address that
    /// belongs to a non-sampled allocation, which is the common case
    /// under the default 1-in-512KiB sampling rate.  Also returns
    /// `None` when profiling is disabled at C-build time.
    ///
    /// Pure read: never mutates allocator state.  Concurrent allocs
    /// and frees are tolerated by the underlying lock-free
    /// `SampledList` snapshot used internally; a sample that fires
    /// after the call begins may or may not be observed.
    ///
    /// # Example
    ///
    /// ```no_run
    /// # #[cfg(feature = "profiling")]
    /// # fn main() {
    /// use snmalloc_rs::SnMalloc;
    ///
    /// let allocator = SnMalloc::new();
    /// // Suppose `addr` came from a PMU sample (Linux perf cycle event).
    /// let addr: *const u8 = core::ptr::null();
    /// if let Some(site) = allocator.lookup_alloc_site(addr) {
    ///     println!(
    ///         "PMU sample at {:p} belongs to alloc {:p}..+{}; alloc-stack {} frames",
    ///         addr,
    ///         site.base_addr,
    ///         site.allocated_size,
    ///         site.frames.len(),
    ///     );
    /// }
    /// # }
    /// # #[cfg(not(feature = "profiling"))]
    /// # fn main() {}
    /// ```
    pub fn lookup_alloc_site(&self, addr: *const u8) -> Option<Frames> {
        // Capacity matches the C++-side cap (SNMALLOC_PROFILE_STACK_FRAMES);
        // the FFI never writes more than this.  Using a Vec lets us hand
        // the buffer to the C call as a mutable pointer; we resize down
        // to the returned length on success.
        let mut buf: Vec<usize> = alloc::vec![0usize; ffi::SN_RUST_PROFILE_STACK_FRAMES];
        let mut base_addr: usize = 0;
        let mut allocated_size: usize = 0;
        let rc = unsafe {
            ffi::sn_rust_profile_lookup_alloc_site(
                addr as usize,
                buf.as_mut_ptr(),
                buf.len(),
                &mut base_addr as *mut usize,
                &mut allocated_size as *mut usize,
            )
        };
        if rc < 0 {
            return None;
        }
        let n = rc as usize;
        // Defensive: the FFI contract caps the write at our buffer
        // capacity, so this branch should never fire -- but a stray
        // mis-sized write would otherwise produce a corrupt frames Vec.
        let n = n.min(buf.len());
        buf.truncate(n);
        let frames: Vec<*const u8> = buf.into_iter().map(|u| u as *const u8).collect();
        Some(Frames {
            frames,
            base_addr: base_addr as *const u8,
            allocated_size,
        })
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

    /// A uniquely-named, deliberately non-inlined function that
    /// captures a real return-address backtrace at its own call
    /// site.  Returning the frames lets the test resolve them
    /// without relying on a `fn` -> code-pointer cast (which on
    /// macOS arm64 returns a stub address that resolves to the
    /// nearest neighbouring symbol, not the function body itself).
    #[cfg(feature = "symbolicate")]
    #[inline(never)]
    fn snmalloc_rs_phase_4_4_symbolize_probe() -> std::vec::Vec<*const u8> {
        let mut frames: std::vec::Vec<*const u8> = std::vec::Vec::new();
        backtrace::trace(|frame| {
            // `ip()` is the instruction pointer of the call site --
            // i.e. an address inside this probe function or its
            // callers.  Recording all of them gives the test a
            // robust signal: at least one frame must resolve back
            // to the probe's own demangled name.
            frames.push(frame.ip() as *const u8);
            true
        });
        frames
    }

    /// `symbolize` resolves a real call-site return address to a
    /// name containing the enclosing function's identifier.  This
    /// is the fundamental smoke test for the symbol backend: if it
    /// fails, no other symbolicator code can possibly work.
    ///
    /// We deliberately capture a live backtrace inside a uniquely-
    /// named function rather than casting a `fn` item to a pointer.
    /// On macOS arm64 in particular, `fn` items lower to a thunk
    /// whose address is *between* two functions in the linker map,
    /// and the symbolicator legitimately reports the neighbour.
    #[cfg(feature = "symbolicate")]
    #[test]
    fn symbolize_resolves_known_function_name() {
        let frames = snmalloc_rs_phase_4_4_symbolize_probe();
        assert!(!frames.is_empty(), "backtrace::trace returned no frames");
        let sample = BtSample {
            alloc_ptr: core::ptr::null(),
            requested_size: 1,
            allocated_size: 1,
            weight: 1,
            stack: frames.clone(),
        };
        let p = HeapProfile::from_samples(vec![sample]);
        let resolved = p.symbolize();
        // At least one resolved frame must mention the probe's
        // identifier.  The exact frame index isn't fixed -- inlining
        // of `backtrace::trace`'s own machinery can vary -- but the
        // probe *itself* is `#[inline(never)]` so it always appears.
        let any_match = frames.iter().any(|addr| {
            resolved
                .get(addr)
                .and_then(|r| r.name.as_deref())
                .map(|name| name.contains("snmalloc_rs_phase_4_4_symbolize_probe"))
                .unwrap_or(false)
        });
        assert!(
            any_match,
            "no resolved frame contained the probe identifier; \
             resolved names: {:?}",
            resolved
                .values()
                .filter_map(|r| r.name.as_deref())
                .collect::<std::vec::Vec<_>>()
        );
    }

    /// `symbolize` on an empty profile is a no-op that returns an
    /// empty map.  This is the contract that lets callers invoke it
    /// unconditionally on the profiling-feature-off build.
    #[cfg(feature = "symbolicate")]
    #[test]
    fn symbolize_empty_profile_is_empty_map() {
        let p = HeapProfile::default();
        let resolved = p.symbolize();
        assert!(resolved.is_empty());
    }

    /// Unresolved frames still appear in the map -- with all metadata
    /// `None`.  This keeps the keyset invariant (every unique frame
    /// in the snapshot is a key) easy to rely on at the call site.
    #[cfg(feature = "symbolicate")]
    #[test]
    fn symbolize_unresolved_frame_has_none_fields() {
        // A pointer that is extremely unlikely to land in any loaded
        // executable's text segment.  Even with ASLR maxed out, the
        // bottom-of-virtual-address-space pages aren't backed by
        // code.
        let addr: *const u8 = 0x1usize as *const u8;
        let sample = BtSample {
            alloc_ptr: core::ptr::null(),
            requested_size: 1,
            allocated_size: 1,
            weight: 1,
            stack: vec![addr],
        };
        let p = HeapProfile::from_samples(vec![sample]);
        let resolved = p.symbolize();
        let frame = resolved.get(&addr).expect("address should be in the map");
        assert!(frame.name.is_none());
        assert!(frame.file.is_none());
        assert!(frame.line.is_none());
        assert_eq!(frame.address, addr);
    }

    /// `write_flamegraph_symbolized` falls back to the hex rendering
    /// for frames whose name does not resolve.  Combined with the
    /// above tests, this proves the renderer is total over arbitrary
    /// frame addresses.
    #[cfg(feature = "symbolicate")]
    #[test]
    fn flamegraph_symbolized_falls_back_to_hex() {
        let addr: *const u8 = 0xabcdusize as *const u8;
        let p = HeapProfile::from_samples(vec![BtSample {
            alloc_ptr: core::ptr::null(),
            requested_size: 64,
            allocated_size: 64,
            weight: 4096,
            stack: vec![addr],
        }]);
        let mut out: std::vec::Vec<u8> = std::vec::Vec::new();
        p.write_flamegraph_symbolized(&mut out).unwrap();
        let text = std::string::String::from_utf8(out).unwrap();
        let lines: std::vec::Vec<&str> = text.lines().collect();
        assert_eq!(lines.len(), 1);
        assert_eq!(lines[0], "0x000000000000abcd 4096");
    }

    /// `write_flamegraph_symbolized` on an empty profile writes
    /// nothing and reports success -- same contract as
    /// `write_flamegraph`.
    #[cfg(feature = "symbolicate")]
    #[test]
    fn flamegraph_symbolized_empty_profile_is_noop() {
        let p = HeapProfile::default();
        let mut out: std::vec::Vec<u8> = std::vec::Vec::new();
        p.write_flamegraph_symbolized(&mut out).unwrap();
        assert!(out.is_empty());
    }
}
