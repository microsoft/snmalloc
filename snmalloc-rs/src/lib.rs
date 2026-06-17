#![no_std]
//! `snmalloc-rs` provides a wrapper for [`microsoft/snmalloc`](https://github.com/microsoft/snmalloc) to make it usable as a global allocator for rust.
//! snmalloc is a research allocator. Its key design features are:
//! - Memory that is freed by the same thread that allocated it does not require any synchronising operations.
//! - Freeing memory in a different thread to initially allocated it, does not take any locks and instead uses a novel message passing scheme to return the memory to the original allocator, where it is recycled.
//! - The allocator uses large ranges of pages to reduce the amount of meta-data required.
//!
//! The benchmark is available at the [paper](https://github.com/microsoft/snmalloc/blob/master/snmalloc.pdf) of `snmalloc`
//! There are three features defined in this crate:
//! - `debug`: Enable the `Debug` mode in `snmalloc`.
//! - `1mib`: Use the `1mib` chunk configuration.
//! - `cache-friendly`: Make the allocator more cache friendly (setting `CACHE_FRIENDLY_OFFSET` to `64` in building the library).
//!
//! The whole library supports `no_std`.
//!
//! To use `snmalloc-rs` add it as a dependency:
//! ```toml
//! # Cargo.toml
//! [dependencies]
//! snmalloc-rs = "0.1.0"
//! ```
//!
//! To set `SnMalloc` as the global allocator add this to your project:
//! ```rust
//! #[global_allocator]
//! static ALLOC: snmalloc_rs::SnMalloc = snmalloc_rs::SnMalloc;
//! ```
//!
//! # Heap profiling
//!
//! With the `profiling` Cargo feature enabled (and the matching C-side
//! `SNMALLOC_PROFILE` build flag, which is set automatically by
//! `snmalloc-sys/build.rs` when the feature is on) `snmalloc-rs` can
//! capture **Poisson-sampled** snapshots of currently-live allocations
//! and emit them in either the collapsed flamegraph format or Google's
//! pprof protobuf.  End-to-end example:
//!
//! ```no_run
//! # #[cfg(feature = "profiling")]
//! # fn main() -> std::io::Result<()> {
//! use snmalloc_rs::{SnMalloc, ProfileConfig};
//! use std::fs::File;
//!
//! let allocator = SnMalloc::new();
//!
//! // Sample once per ~512 KiB of allocation (low-overhead default).
//! allocator.configure_profiling(ProfileConfig::with_sampling_rate(524_288));
//!
//! // ... run the workload you want to profile ...
//!
//! let profile = allocator.snapshot();
//! println!("captured {} samples, ~{} bytes live",
//!     profile.len(), profile.total_allocated_bytes());
//!
//! // Folded-stack format -- feed to `inferno-flamegraph` or speedscope.
//! let mut f = File::create("heap.folded")?;
//! profile.write_flamegraph(&mut f)?;
//! # Ok(())
//! # }
//! # #[cfg(not(feature = "profiling"))]
//! # fn main() {}
//! ```
//!
//! See [`HeapProfile::write_flamegraph`] for the folded-stack format and
//! [`HeapProfile::write_pprof`] for the pprof protobuf format.  For
//! continuous (streaming) sampling rather than one-shot snapshots see
//! [`ProfilingSession::start`].
extern crate snmalloc_sys as ffi;

use core::{
    alloc::{GlobalAlloc, Layout},
    ptr::NonNull,
};

/// Safe Rust wrapper over the `sn_rust_profile_*` FFI surface.
///
/// The module is compiled unconditionally so that downstream code can
/// always refer to [`HeapProfile`] / [`BtSample`] / the snapshot
/// methods on [`SnMalloc`] without conditional compilation.  When the
/// `profiling` Cargo feature (and the matching C-side
/// `SNMALLOC_PROFILE` build flag) are not enabled, the FFI returns
/// no-op responses and the safe wrappers degrade to empty results --
/// see [`profile`] for details.
pub mod profile;

/// Runtime configuration helpers (Phase 4.5): a typed [`ProfileConfig`]
/// struct plus an env-var-driven initializer
/// ([`SnMalloc::init_profiling_from_env`]) so binaries can opt into
/// heap profiling at the command line without recompiling.  See
/// [`config`] for the env-var contract.
pub mod config;

/// Text-dump API (Phase 9.6) -- safe Rust wrapper around the
/// `snmalloc_dump_stats_to_buffer` C ABI.  Two-phase
/// (size-query + alloc + fill) write into a borrowed
/// `std::io::Write` sink.  See [`SnMalloc::dump_stats`].
pub mod stats_dump;

/// Google pprof Profile protobuf encoder (Phase 6.1).
///
/// Hand-rolled protobuf3 encoder (no `prost` dependency) covering
/// the subset of [`pprof`](https://github.com/google/pprof) the
/// snmalloc heap profile maps onto: two sample-type axes
/// (`alloc_objects`/count and `alloc_space`/bytes) plus a per-stack
/// location/function chain.  Exposed externally via the
/// [`HeapProfile::write_pprof`] convenience wrapper.
pub(crate) mod pprof;

/// Streaming-mode safe Rust wrapper (Phase 5.2).
///
/// Lifts the C-level `sn_rust_profile_streaming_*` FFI surface into
/// an RAII [`streaming::ProfilingSession`] handle plus a borrowed
/// [`streaming::StreamSample`] view of each broadcast sample.  Only
/// compiled when the `profiling` Cargo feature is on, since the
/// underlying FFI symbols only do useful work in that configuration
/// and the wrapper depends on `std::sync` primitives.
#[cfg(feature = "profiling")]
pub mod streaming;

/// Criterion bench-profiling helper (ticket 86aj2dww6).
///
/// Provides [`criterion::bench_with_profile`] and
/// [`criterion::bench_with_profile_batched`], thin wrappers around a
/// single [`streaming::ProfilingSession`] that surround the criterion
/// measurement loop.  Gated on `feature = "profiling"` AND
/// `feature = "criterion-integration"` so that neither criterion nor
/// flate2 are pulled into a default build.
#[cfg(all(feature = "profiling", feature = "criterion-integration"))]
pub mod criterion;

pub use profile::{BtSample, Frames, HeapProfile, HotSite, HotSpotKey, Weight};

#[cfg(feature = "symbolicate")]
pub use profile::clear_symbol_cache;
pub use config::{ProfileConfig, ENV_PROFILE_ENABLE, ENV_PROFILE_RATE};

/// Re-export of the Phase 9.1 wire-format version constant.  Lets
/// downstream consumers compare against `FullAllocStats::version`
/// without depending on the `snmalloc-sys` crate directly.
///
/// Bumped to `2` in Phase 11.4 with the addition of the free-chunk
/// histogram in `FullAllocStats.reserved[0..16]`; see
/// [`SnMalloc::full_stats`] and [`FullAllocStats::free_chunk_histogram`].
#[cfg(feature = "stats-basic")]
pub use ffi::SNMALLOC_FULL_STATS_VERSION;

/// Re-export of the Phase 11.4 free-chunk histogram bucket count.
/// Equal to `16`.  See [`FullAllocStats::free_chunk_histogram`].
#[cfg(feature = "stats-basic")]
pub use ffi::SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS;

#[cfg(feature = "profiling")]
pub use streaming::{ProfilingSession, StreamSample, StreamingError};

/// Memory usage statistics from the snmalloc backend.
///
/// These are range-level figures (slab/chunk granularity) reflecting bytes
/// reserved from the OS, not the count of live individual allocations.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct AllocStats {
    /// Bytes currently reserved from the OS.
    pub current_memory_usage: usize,
    /// High-water mark of `current_memory_usage`.
    pub peak_memory_usage: usize,
}

/// Aggregated allocator telemetry snapshot (Phase 9.1 scaffold).
///
/// Idiomatic Rust mirror of `struct snmalloc_full_stats` from the C
/// header `src/snmalloc/global/stats_export.h`.  Field semantics are
/// documented on the FFI struct
/// [`snmalloc_sys::snmalloc_full_stats`]; the Rust mirror exists so
/// callers don't need to depend on the `snmalloc-sys` crate directly.
///
/// At the scaffold stage only `version`, `bytes_in_use`, and
/// `peak_bytes_in_use` carry meaningful values; every other field is
/// zero.  Subsequent Phase 9 tickets populate the remaining fields:
///
///   * 9.2 -- fast/slow path alloc/dealloc and cross-thread message
///            counters;
///   * 9.3 -- per-size-class live / cumulative byte and count
///            histograms;
///   * 9.4 -- `bytes_mapped` / `bytes_committed` /
///            `bytes_decommitted_to_os`;
///   * 9.5 -- `lifetime_buckets_ns` allocation-lifetime histogram.
///
/// The struct is `Copy` and `Default` (all-zero) so callers can
/// trivially compute diffs across two snapshots.  Available only
/// when the `stats-basic` (or, by implication, the `stats-full` or
/// legacy `stats`) Cargo feature is on; without one of those
/// `full_stats()` does not exist (compile-time gate, not a
/// runtime-zero stub).
///
/// Phase 11.6 -- tiered stats.  The struct layout is identical
/// across the two tiers (ABI preserved); fields that the BASIC
/// tier does not maintain simply read as zero.  Specifically:
///
///   * BASIC populates: `version`, `bytes_in_use`,
///     `peak_bytes_in_use`, `bytes_mapped`, `bytes_committed`,
///     `bytes_decommitted_to_os`, `fast_path_allocs`,
///     `slow_path_allocs`, `fast_path_deallocs`,
///     `remote_deallocs`, `message_queue_drains`,
///     `cross_thread_messages_received`, and the
///     `LargeBuddyRange` free-chunk histogram via
///     [`FullAllocStats::free_chunk_histogram`].
///   * FULL adds: `total_live_bytes_by_class`,
///     `total_live_count_by_class`, `cumulative_alloc_by_class`,
///     `cumulative_dealloc_by_class`, and
///     `lifetime_buckets_ns` (the lifetime histogram, which
///     additionally requires `SNMALLOC_PROFILE` to be on at the
///     C++ level for the bucket bumps to fire).
///
/// `Default` is implemented manually rather than derived because
/// stable Rust's `derive(Default)` does not yet cover fixed-size
/// arrays larger than 32 elements; the explicit impl below
/// hand-writes the all-zero initializer for the per-size-class
/// histograms (64 slots each) and the lifetime histogram (32 slots).
#[cfg(feature = "stats-basic")]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FullAllocStats {
    /// Wire-format version of the snapshot (the producer's
    /// `SNMALLOC_FULL_STATS_VERSION`).  Callers MAY compare against
    /// [`ffi::SNMALLOC_FULL_STATS_VERSION`] to detect newer fields they
    /// don't yet know about; the prefix layout is stable.
    pub version: u32,
    /// Bytes currently reserved from the OS (range granularity, same
    /// source as [`SnMalloc::memory_stats`]).
    pub bytes_in_use: u64,
    /// High-water mark of `bytes_in_use`.
    pub peak_bytes_in_use: u64,
    /// Phase 9.4 -- bytes currently mapped from the OS.
    pub bytes_mapped: u64,
    /// Phase 9.4 -- bytes currently committed (writable / RSS-eligible).
    pub bytes_committed: u64,
    /// Phase 9.4 -- cumulative bytes decommitted back to the OS.
    pub bytes_decommitted_to_os: u64,
    /// Phase 9.2 -- allocations satisfied entirely on the fast path.
    pub fast_path_allocs: u64,
    /// Phase 9.2 -- allocations that fell through to the slow path.
    pub slow_path_allocs: u64,
    /// Phase 9.2 -- deallocations satisfied entirely on the fast path.
    pub fast_path_deallocs: u64,
    /// Phase 9.2 -- deallocations routed to a remote allocator.
    pub remote_deallocs: u64,
    /// Phase 9.2 -- cross-thread message-queue drain count.
    pub message_queue_drains: u64,
    /// Phase 9.2 -- total cross-thread messages received.
    pub cross_thread_messages_received: u64,
    /// Phase 9.3 -- live bytes by size class.
    pub total_live_bytes_by_class: [u64; ffi::SNMALLOC_FULL_STATS_SIZECLASS_SLOTS],
    /// Phase 9.3 -- live object count by size class.
    pub total_live_count_by_class: [u64; ffi::SNMALLOC_FULL_STATS_SIZECLASS_SLOTS],
    /// Phase 9.3 -- cumulative allocations by size class.
    pub cumulative_alloc_by_class: [u64; ffi::SNMALLOC_FULL_STATS_SIZECLASS_SLOTS],
    /// Phase 9.3 -- cumulative deallocations by size class.
    pub cumulative_dealloc_by_class: [u64; ffi::SNMALLOC_FULL_STATS_SIZECLASS_SLOTS],
    /// Phase 9.5 -- log2-spaced allocation-lifetime histogram.
    pub lifetime_buckets_ns: [u64; ffi::SNMALLOC_FULL_STATS_LIFETIME_BUCKETS],
    /// Forward-compat reserve pool.  As of `SNMALLOC_FULL_STATS_VERSION = 2`
    /// (Phase 11.4) `reserved[0..16]` carries the log2-bucketed
    /// `LargeBuddyRange` free-chunk histogram; prefer the typed
    /// accessor [`FullAllocStats::free_chunk_histogram`] for that view.
    /// Slots `reserved[16..]` remain zero and are reserved for future
    /// additive extensions.
    pub reserved: [u64; ffi::SNMALLOC_FULL_STATS_RESERVED_SLOTS],
}

#[cfg(feature = "stats-basic")]
impl FullAllocStats {
    /// Return the Phase 11.4 free-chunk histogram from
    /// `reserved[0..16]` as a typed array.
    ///
    /// Bucket `i` is the count of currently-free chunks of size
    /// `1 << (MIN_CHUNK_BITS + i)` bytes held inside any
    /// `LargeBuddyRange` Buddy at the moment the snapshot was taken;
    /// `MIN_CHUNK_BITS` is `14` (16 KiB) on the default build, so the
    /// 16 buckets cover sizes from 16 KiB up to `16 KiB << 15` = 512 MiB.
    ///
    /// Returns an all-zero array when the producer is older than
    /// `SNMALLOC_FULL_STATS_VERSION = 2` (the slot pool reads as zero
    /// in that case).
    #[inline]
    pub fn free_chunk_histogram(
        &self,
    ) -> [u64; ffi::SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS] {
        let mut out = [0u64; ffi::SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS];
        out.copy_from_slice(
            &self.reserved[..ffi::SNMALLOC_FULL_STATS_FREECHUNK_BUCKETS],
        );
        out
    }
}

#[cfg(feature = "stats-basic")]
impl Default for FullAllocStats {
    /// All-zero default, matching the post-`memset` state of a fresh
    /// `snmalloc_full_stats` on the C side.  Useful as a baseline when
    /// computing deltas across two snapshots; the
    /// `SNMALLOC_FULL_STATS_VERSION` constant is intentionally NOT
    /// populated here so a `Default::default()` value is trivially
    /// distinguishable from a real snapshot.
    fn default() -> Self {
        Self {
            version: 0,
            bytes_in_use: 0,
            peak_bytes_in_use: 0,
            bytes_mapped: 0,
            bytes_committed: 0,
            bytes_decommitted_to_os: 0,
            fast_path_allocs: 0,
            slow_path_allocs: 0,
            fast_path_deallocs: 0,
            remote_deallocs: 0,
            message_queue_drains: 0,
            cross_thread_messages_received: 0,
            total_live_bytes_by_class: [0u64; ffi::SNMALLOC_FULL_STATS_SIZECLASS_SLOTS],
            total_live_count_by_class: [0u64; ffi::SNMALLOC_FULL_STATS_SIZECLASS_SLOTS],
            cumulative_alloc_by_class: [0u64; ffi::SNMALLOC_FULL_STATS_SIZECLASS_SLOTS],
            cumulative_dealloc_by_class: [0u64; ffi::SNMALLOC_FULL_STATS_SIZECLASS_SLOTS],
            lifetime_buckets_ns: [0u64; ffi::SNMALLOC_FULL_STATS_LIFETIME_BUCKETS],
            reserved: [0u64; ffi::SNMALLOC_FULL_STATS_RESERVED_SLOTS],
        }
    }
}

#[derive(Debug, Copy, Clone)]
#[repr(C)]
pub struct SnMalloc;

unsafe impl Send for SnMalloc {}
unsafe impl Sync for SnMalloc {}

impl SnMalloc {
    #[inline(always)]
    pub const fn new() -> Self {
        Self
    }

    /// Returns the available bytes in a memory block.
    #[inline(always)]
    pub fn usable_size(&self, ptr: *const u8) -> Option<usize> {
        match ptr.is_null() {
            true => None,
            false => Some(unsafe { ffi::sn_rust_usable_size(ptr.cast()) })
        }
    }

    /// Returns current and peak OS-level memory reservation statistics.
    /// See [`AllocStats`] for what the values measure.
    pub fn memory_stats() -> AllocStats {
        let mut current = 0usize;
        let mut peak = 0usize;
        unsafe { ffi::sn_rust_statistics(&mut current, &mut peak) };
        AllocStats { current_memory_usage: current, peak_memory_usage: peak }
    }

    /// Capture a full allocator-telemetry snapshot (Phase 9.1 scaffold).
    ///
    /// Calls the underlying `snmalloc_get_full_stats` C ABI and copies
    /// every field across into the idiomatic Rust mirror
    /// [`FullAllocStats`].  Only `version`, `bytes_in_use`, and
    /// `peak_bytes_in_use` carry meaningful values at the scaffold
    /// stage; all other fields read as zero and will be populated by
    /// the Phase 9 wave-2 tickets (9.2 / 9.3 / 9.4 / 9.5).
    ///
    /// No allocator state is mutated -- the call is a pure read backed
    /// by atomic counters and safe to invoke from any thread.
    ///
    /// Gated behind the `stats` Cargo feature so consumers that don't
    /// want the extra telemetry surface get a hard compile error
    /// referring to this method, rather than silently linking against
    /// a zero-returning stub.
    #[cfg(feature = "stats-basic")]
    pub fn full_stats() -> FullAllocStats {
        // SAFETY: the C function fills `raw` in full via memset+writes
        // before returning; no field is left uninitialised.  We pass
        // a stack-local pointer with the correct alignment.
        let mut raw: ffi::snmalloc_full_stats = unsafe { core::mem::zeroed() };
        unsafe { ffi::snmalloc_get_full_stats(&mut raw) };

        FullAllocStats {
            version: raw.version,
            bytes_in_use: raw.bytes_in_use,
            peak_bytes_in_use: raw.peak_bytes_in_use,
            bytes_mapped: raw.bytes_mapped,
            bytes_committed: raw.bytes_committed,
            bytes_decommitted_to_os: raw.bytes_decommitted_to_os,
            fast_path_allocs: raw.fast_path_allocs,
            slow_path_allocs: raw.slow_path_allocs,
            fast_path_deallocs: raw.fast_path_deallocs,
            remote_deallocs: raw.remote_deallocs,
            message_queue_drains: raw.message_queue_drains,
            cross_thread_messages_received: raw.cross_thread_messages_received,
            total_live_bytes_by_class: raw.total_live_bytes_by_class,
            total_live_count_by_class: raw.total_live_count_by_class,
            cumulative_alloc_by_class: raw.cumulative_alloc_by_class,
            cumulative_dealloc_by_class: raw.cumulative_dealloc_by_class,
            lifetime_buckets_ns: raw.lifetime_buckets_ns,
            reserved: raw.reserved,
        }
    }

    // ------------------------------------------------------------------
    // Phase 9.7 -- runtime tunables.
    //
    // Three process-wide knobs (Poisson sample interval, chunk decay
    // window, per-thread local-cache cap) that used to be compile-time
    // constants.  Exposed unconditionally -- NOT gated on the `stats`
    // or `profiling` features -- because the underlying C ABI shims
    // are always linked into the Rust archive, and the tunables are
    // useful in every build flavour.  Setting the sample interval in
    // a non-profile build is harmless (stored only); rebuilding with
    // `profiling` on then picks it up automatically.
    //
    // All six methods are safe to call from any thread at any point in
    // the process lifetime, including before the first allocation.

    /// Set the mean Poisson sampling interval for the heap profiler,
    /// in bytes.  Zero disables sampling.  Mirrors into the profiler's
    /// `Sampler::set_sampling_rate` when the underlying C build has
    /// `SNMALLOC_PROFILE` defined (the `profiling` Cargo feature
    /// sets that flag); otherwise stored only.
    ///
    /// This is the same knob that
    /// `sn_rust_profile_set_sampling_rate` controls in profile-feature
    /// builds; it is exposed independently so non-profile builds can
    /// stage a value before the profiler is compiled in.
    #[inline]
    pub fn set_sample_interval(bytes: u64) {
        unsafe { ffi::snmalloc_set_sample_interval(bytes) }
    }

    /// Get the current mean Poisson sampling interval, in bytes.
    #[inline]
    pub fn sample_interval() -> u64 {
        unsafe { ffi::snmalloc_get_sample_interval() }
    }

    /// Set the chunk decay window, in milliseconds.  Zero is a valid
    /// value.  The backend read-side hook for this tunable is a
    /// follow-up; at present the setter stores only.
    #[inline]
    pub fn set_decay_rate(milliseconds: u32) {
        unsafe { ffi::snmalloc_set_decay_rate(milliseconds) }
    }

    /// Get the current chunk decay window, in milliseconds.
    #[inline]
    pub fn decay_rate() -> u32 {
        unsafe { ffi::snmalloc_get_decay_rate() }
    }

    /// Set the per-thread local-cache cap, in bytes.  The per-thread
    /// cache read-side hook is a follow-up; at present the setter
    /// stores only.
    #[inline]
    pub fn set_max_local_cache(bytes: u64) {
        unsafe { ffi::snmalloc_set_max_local_cache(bytes) }
    }

    /// Get the current per-thread local-cache cap, in bytes.
    #[inline]
    pub fn max_local_cache() -> u64 {
        unsafe { ffi::snmalloc_get_max_local_cache() }
    }


    /// Allocates memory with the given layout, returning a non-null pointer on success
    #[inline(always)]
    pub fn alloc_aligned(&self, layout: Layout) -> Option<NonNull<u8>> {
        match layout.size() {
            0 => NonNull::new(layout.align() as *mut u8),
            size => NonNull::new(unsafe { ffi::sn_rust_alloc(layout.align(), size) }.cast())
        }
    }
}

unsafe impl GlobalAlloc for SnMalloc {
    /// Allocate the memory with the given alignment and size.
    /// On success, it returns a pointer pointing to the required memory address.
    /// On failure, it returns a null pointer.
    /// The client must assure the following things:
    /// - `alignment` is greater than zero
    /// - Other constrains are the same as the rust standard library.
    ///
    /// The program may be forced to abort if the constrains are not full-filled.
    #[inline(always)]
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        match layout.size() {
            0 => layout.align() as *mut u8,
            size => ffi::sn_rust_alloc(layout.align(), size).cast()
        }
    }

    /// De-allocate the memory at the given address with the given alignment and size.
    /// The client must assure the following things:
    /// - the memory is acquired using the same allocator and the pointer points to the start position.
    /// - Other constrains are the same as the rust standard library.
    ///
    /// The program may be forced to abort if the constrains are not full-filled.
    #[inline(always)]
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if layout.size() != 0 {
            ffi::sn_rust_dealloc(ptr as _, layout.align(), layout.size());
        }
    }

    /// Behaves like alloc, but also ensures that the contents are set to zero before being returned.
    #[inline(always)]
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        match layout.size() {
            0 => layout.align() as *mut u8,
            size => ffi::sn_rust_alloc_zeroed(layout.align(), size).cast()
        }
    }

    /// Re-allocate the memory at the given address with the given alignment and size.
    /// On success, it returns a pointer pointing to the required memory address.
    /// The memory content within the `new_size` will remains the same as previous.
    /// On failure, it returns a null pointer. In this situation, the previous memory is not returned to the allocator.
    /// The client must assure the following things:
    /// - the memory is acquired using the same allocator and the pointer points to the start position
    /// - `alignment` fulfills all the requirements as `rust_alloc`
    /// - Other constrains are the same as the rust standard library.
    ///
    /// The program may be forced to abort if the constrains are not full-filled.
    #[inline(always)]
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        match new_size {
            0 => {
                self.dealloc(ptr, layout);
                layout.align() as *mut u8
            }
            new_size if layout.size() == 0 => {
                self.alloc(Layout::from_size_align_unchecked(new_size, layout.align()))
            }
            _ => ffi::sn_rust_realloc(ptr.cast(), layout.align(), layout.size(), new_size).cast()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn allocation_lifecycle() {
        let alloc = SnMalloc::new();
        unsafe {
            let layout = Layout::from_size_align(8, 8).unwrap();
            
            // Test regular allocation
            let ptr = alloc.alloc(layout);
            alloc.dealloc(ptr, layout);

            // Test zeroed allocation
            let ptr = alloc.alloc_zeroed(layout);
            alloc.dealloc(ptr, layout);

            // Test reallocation
            let ptr = alloc.alloc(layout);
            let ptr = alloc.realloc(ptr, layout, 16);
            alloc.dealloc(ptr, layout);

            // Test large allocation
            let large_layout = Layout::from_size_align(1 << 20, 32).unwrap();
            let ptr = alloc.alloc(large_layout);
            alloc.dealloc(ptr, large_layout);
        }
    }
    #[test]
    fn it_frees_allocated_memory() {
        unsafe {
            let layout = Layout::from_size_align(8, 8).unwrap();
            let alloc = SnMalloc;

            let ptr = alloc.alloc(layout);
            alloc.dealloc(ptr, layout);
        }
    }

    #[test]
    fn it_frees_zero_allocated_memory() {
        unsafe {
            let layout = Layout::from_size_align(8, 8).unwrap();
            let alloc = SnMalloc;

            let ptr = alloc.alloc_zeroed(layout);
            alloc.dealloc(ptr, layout);
        }
    }

    #[test]
    fn it_frees_reallocated_memory() {
        unsafe {
            let layout = Layout::from_size_align(8, 8).unwrap();
            let alloc = SnMalloc;

            let ptr = alloc.alloc(layout);
            let ptr = alloc.realloc(ptr, layout, 16);
            alloc.dealloc(ptr, layout);
        }
    }

    #[test]
    fn it_frees_large_alloc() {
        unsafe {
            let layout = Layout::from_size_align(1 << 20, 32).unwrap();
            let alloc = SnMalloc;

            let ptr = alloc.alloc(layout);
            alloc.dealloc(ptr, layout);
        }
    }

    #[test]
    fn test_usable_size() {
        let alloc = SnMalloc::new();
        unsafe {
            let layout = Layout::from_size_align(8, 8).unwrap();
            let ptr = alloc.alloc(layout);
            let usz = alloc.usable_size(ptr).expect("usable_size returned None");
            alloc.dealloc(ptr, layout);
            assert!(usz >= 8);
        }
    }
}
