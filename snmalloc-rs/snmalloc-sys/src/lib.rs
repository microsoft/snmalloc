#![no_std]
#![allow(non_camel_case_types)]

use core::ffi::c_void;

/// Stack-frame depth captured per sampled allocation.  Must match
/// `SNMALLOC_PROFILE_STACK_FRAMES` in `src/snmalloc/override/rust_profile.h`
/// (default 32).  Both ends use the same constant so the `SnRustProfileRawSample`
/// layout is bit-for-bit identical across the FFI boundary.
pub const SN_RUST_PROFILE_STACK_FRAMES: usize = 32;

extern "C" {
    /// Allocate the memory with the given alignment and size.
    /// On success, it returns a pointer pointing to the required memory address.
    /// On failure, it returns a null pointer.
    /// The client must assure the following things:
    /// - `alignment` is greater than zero
    /// - `alignment` is a power of 2
    /// The program may be forced to abort if the constrains are not full-filled.
    pub fn sn_rust_alloc(alignment: usize, size: usize) -> *mut c_void;

    /// De-allocate the memory at the given address with the given alignment and size.
    /// The client must assure the following things:
    /// - the memory is acquired using the same allocator and the pointer points to the start position.
    /// - `alignment` and `size` is the same as allocation
    /// The program may be forced to abort if the constrains are not full-filled.
    pub fn sn_rust_dealloc(ptr: *mut c_void, alignment: usize, size: usize) -> c_void;

    /// Behaves like rust_alloc, but also ensures that the contents are set to zero before being returned.
    pub fn sn_rust_alloc_zeroed(alignment: usize, size: usize) -> *mut c_void;

    /// Re-allocate the memory at the given address with the given alignment and size.
    /// On success, it returns a pointer pointing to the required memory address.
    /// The memory content within the `new_size` will remains the same as previous.
    /// On failure, it returns a null pointer. In this situation, the previous memory is not returned to the allocator.
    /// The client must assure the following things:
    /// - the memory is acquired using the same allocator and the pointer points to the start position
    /// - `alignment` and `old_size` is the same as allocation
    /// - `alignment` fulfills all the requirements as `rust_alloc`
    /// The program may be forced to abort if the constrains are not full-filled.
    pub fn sn_rust_realloc(
        ptr: *mut c_void,
        alignment: usize,
        old_size: usize,
        new_size: usize,
    ) -> *mut c_void;

    /// Return the available bytes in a memory block.
    pub fn sn_rust_usable_size(p: *const c_void) -> usize;

    /// Write current and peak OS-level memory reservation bytes into the given pointers.
    pub fn sn_rust_statistics(
        current_memory_usage: *mut usize,
        peak_memory_usage: *mut usize,
    );
}

#[cfg(feature = "libc-api")]
extern "C" {
    /// Allocate `count` items of `size` length each.
    /// Returns `null` if `count * size` overflows or on out-of-memory.
    /// All items are initialized to zero.
    pub fn sn_calloc(count: usize, size: usize) -> *mut c_void;

    /// Allocate `size` bytes.
    /// Returns pointer to the allocated memory or null if out of memory.
    /// Returns a unique pointer if called with `size` 0.
    pub fn sn_malloc(size: usize) -> *mut c_void;

    /// Re-allocate memory to `newsize` bytes.
    /// Return pointer to the allocated memory or null if out of memory. If null
    /// is returned, the pointer `p` is not freed. Otherwise the original
    /// pointer is either freed or returned as the reallocated result (in case
    /// it fits in-place with the new size).
    /// If `p` is null, it behaves as [`sn_malloc`]. If `newsize` is larger than
    /// the original `size` allocated for `p`, the bytes after `size` are
    /// uninitialized.
    pub fn sn_realloc(p: *mut c_void, newsize: usize) -> *mut c_void;

    /// Free previously allocated memory.
    /// The pointer `p` must have been allocated before (or be null).
    pub fn sn_free(p: *mut c_void);

    /// Return the available bytes in a memory block.
    pub fn sn_malloc_usable_size(p: *const c_void) -> usize;
    
}

/// One sampled allocation, mirrored bit-for-bit from
/// `struct SnRustProfileRawSample` in `src/snmalloc/override/rust_profile.h`.
///
/// `repr(C)` keeps the layout pinned to the C side; the inline stack array
/// is sized by `SN_RUST_PROFILE_STACK_FRAMES`, which must stay in lockstep
/// with the C `SNMALLOC_PROFILE_STACK_FRAMES` macro.  When the underlying
/// snmalloc build was configured with `SNMALLOC_PROFILE=OFF` this struct
/// is still well-defined; the snapshot calls will simply not produce any
/// samples to populate it.
///
/// The struct is exposed unconditionally (independent of the Rust
/// `profiling` Cargo feature) because the matching C symbols in
/// `rust.cc` are always linked -- they degrade to no-op stubs when
/// `SNMALLOC_PROFILE` is undefined.  Keeping the type always-available
/// lets higher-level Rust wrappers expose a uniform safe API surface
/// that compiles in both feature-on and feature-off builds.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct SnRustProfileRawSample {
    /// Pointer returned by the original alloc.  May be null.
    pub alloc_ptr: *mut c_void,
    /// Size requested by the caller (bytes).
    pub requested_size: usize,
    /// Size actually returned (sizeclass-rounded).
    pub allocated_size: usize,
    /// Bytes-of-request weight (Poisson unbiased estimator).
    pub weight: usize,
    /// Number of valid entries in `stack` (0..=SN_RUST_PROFILE_STACK_FRAMES).
    pub stack_depth: u32,
    /// Captured return addresses, innermost first.  Entries beyond
    /// `stack_depth` are unspecified.
    pub stack: [*mut c_void; SN_RUST_PROFILE_STACK_FRAMES],
}

// The `sn_rust_profile_*` C symbols are always exported by
// `src/snmalloc/override/rust.cc` -- when `SNMALLOC_PROFILE` is
// undefined they degrade to no-op stubs that return `0` / `false` /
// `nullptr`.  Exposing the Rust extern block unconditionally lets the
// higher-level `snmalloc-rs` crate expose a uniform safe API in both
// `profiling`-feature-on and `profiling`-feature-off builds (per the
// Phase 4.1 contract: `profiling_supported()` returns `false` and
// `snapshot()` returns an empty profile when the C build is OFF).
extern "C" {
    /// Returns `true` iff this build of snmalloc was compiled with
    /// `SNMALLOC_PROFILE=ON`.  When `false`, every other `sn_rust_profile_*`
    /// call is a no-op or returns zero / null.
    pub fn sn_rust_profile_supported() -> bool;

    /// Set the mean sampling interval, in bytes.  Zero disables sampling.
    /// No-op when `sn_rust_profile_supported()` is false.
    pub fn sn_rust_profile_set_sampling_rate(bytes: usize);

    /// Get the current mean sampling interval, in bytes.  Returns 0 when
    /// `sn_rust_profile_supported()` is false.
    pub fn sn_rust_profile_get_sampling_rate() -> usize;

    /// Begin a snapshot of the currently-live sampled allocations.  The
    /// returned opaque handle must eventually be released via
    /// [`sn_rust_profile_snapshot_end`].  May return null if profiling is
    /// disabled or the snapshot allocation itself failed.
    pub fn sn_rust_profile_snapshot_begin() -> *mut c_void;

    /// Number of samples in the snapshot identified by `handle`.  Returns
    /// 0 for a null handle.
    pub fn sn_rust_profile_snapshot_count(handle: *mut c_void) -> usize;

    /// Copy sample at index `idx` into `*out`.  Returns `false` when
    /// profiling is disabled, the handle is null, `out` is null, or `idx`
    /// is out of range.
    pub fn sn_rust_profile_snapshot_get(
        handle: *mut c_void,
        idx: usize,
        out: *mut SnRustProfileRawSample,
    ) -> bool;

    /// Release the snapshot allocated by
    /// [`sn_rust_profile_snapshot_begin`].  Safe to call with a null
    /// handle.
    pub fn sn_rust_profile_snapshot_end(handle: *mut c_void);
}

// Streaming-mode broadcast (Phase 5.1): a single user callback is invoked
// once per sampled allocation, off the hot path of `record_alloc`.  The C
// implementation enforces a single registered callback at a time; the
// safe Rust wrapper in `snmalloc-rs` layers a `Mutex`-protected
// `Box<dyn Fn>` on top to expose a borrowed view of the raw sample
// (`StreamSample`) and an RAII `ProfilingSession` handle.
//
// These extern decls are gated on the `profiling` Cargo feature so the
// linker only references the streaming symbols in feature-on builds.
// The feature-off (`SNMALLOC_PROFILE` undefined) C stubs still export
// `sn_rust_profile_streaming_start` / `..._stop` returning `-1`, but
// the safe Rust layer never invokes them in that configuration -- the
// entire `streaming` module is itself `cfg`-gated.
#[cfg(feature = "profiling")]
extern "C" {
    /// Register `cb` as the single streaming-mode broadcast handler.
    /// Returns `0` on success or `-1` if a handler is already
    /// registered, `cb` is null, or the underlying broadcast slot is
    /// full.  When `sn_rust_profile_supported()` is false the call is
    /// a no-op that returns `-1`.
    pub fn sn_rust_profile_streaming_start(
        cb: unsafe extern "C" fn(sample: *const SnRustProfileRawSample),
    ) -> core::ffi::c_int;

    /// Unregister the currently-registered streaming broadcast
    /// handler.  Returns `0` on success or `-1` if no handler was
    /// registered.  When `sn_rust_profile_supported()` is false the
    /// call is a no-op that returns `-1`.
    pub fn sn_rust_profile_streaming_stop() -> core::ffi::c_int;
}

#[cfg(test)]
mod rust_tests {
    use super::*;

    #[test]
    fn it_zero_allocs_correctly() {
        let ptr = unsafe { sn_rust_alloc_zeroed(8, 1024) } as *mut u8 as *mut [u8; 1024];
        unsafe {
            assert!((*ptr).iter().all(|x| *x == 0));
        };
        unsafe { sn_rust_dealloc(ptr as *mut c_void, 8, 1024) };
    }

    #[test]
    fn it_frees_memory_malloc() {
        let ptr = unsafe { sn_rust_alloc(8, 8) } as *mut u8;
        unsafe {
            *ptr = 127;
            assert_eq!(*ptr, 127)
        };
        unsafe { sn_rust_dealloc(ptr as *mut c_void, 8, 8) };
    }

    #[test]
    fn it_reallocs_correctly() {
        let mut ptr = unsafe { sn_rust_alloc(8, 8) } as *mut u8;
        unsafe {
            *ptr = 127;
            assert_eq!(*ptr, 127)
        };
        ptr = unsafe { sn_rust_realloc(ptr as *mut c_void, 8, 8, 16) } as *mut u8;
        unsafe { assert_eq!(*ptr, 127) };
        unsafe { sn_rust_dealloc(ptr as *mut c_void, 8, 16) };
    }

    #[test]
    fn it_calculates_usable_size() {
        let ptr = unsafe { sn_rust_alloc(32, 8) } as *mut u8;
        let usable_size = unsafe { sn_rust_usable_size(ptr as *mut c_void) };
        assert!(
            usable_size >= 32,
            "usable_size should at least equal to the allocated size"
        );
        unsafe { sn_rust_dealloc(ptr as *mut c_void, 32, 8) };
    }
}

#[cfg(all(test, feature = "profiling"))]
mod profile_tests {
    use super::*;
    use core::ptr;

    /// Smoke test: with the `profiling` feature on, the snmalloc-sys
    /// build.rs propagates `SNMALLOC_PROFILE=ON` to the cmake build, so
    /// the C side must report support and the snapshot lifecycle must be
    /// callable end-to-end.
    #[test]
    fn supported_when_feature_enabled() {
        let ok = unsafe { sn_rust_profile_supported() };
        assert!(
            ok,
            "sn_rust_profile_supported() must return true when the \
             `profiling` cargo feature wires SNMALLOC_PROFILE=ON"
        );
    }

    #[test]
    fn sampling_rate_roundtrip() {
        unsafe {
            let original = sn_rust_profile_get_sampling_rate();
            sn_rust_profile_set_sampling_rate(123_456);
            assert_eq!(sn_rust_profile_get_sampling_rate(), 123_456);
            // Restore so we don't perturb other tests in the same process.
            sn_rust_profile_set_sampling_rate(original);
        }
    }

    #[test]
    fn snapshot_lifecycle_is_safe() {
        unsafe {
            let h = sn_rust_profile_snapshot_begin();
            // count() / get() / end() must all tolerate either a valid
            // handle or null (in case the snapshot allocation itself
            // failed).  The exact sample count is racy, but the calls
            // must not crash.
            let n = sn_rust_profile_snapshot_count(h);
            if n > 0 && !h.is_null() {
                let mut sample = SnRustProfileRawSample {
                    alloc_ptr: ptr::null_mut(),
                    requested_size: 0,
                    allocated_size: 0,
                    weight: 0,
                    stack_depth: 0,
                    stack: [ptr::null_mut(); SN_RUST_PROFILE_STACK_FRAMES],
                };
                assert!(sn_rust_profile_snapshot_get(h, 0, &mut sample));
                // Out-of-range index must report failure.
                assert!(!sn_rust_profile_snapshot_get(h, n, &mut sample));
            }
            sn_rust_profile_snapshot_end(h);
        }
    }
}

#[cfg(all(test, feature = "libc-api"))]
mod libc_tests {
    use super::*;

    #[test]
    fn it_frees_memory_sn_malloc() {
        let ptr = unsafe { sn_malloc(8) } as *mut u8;
        unsafe { sn_free(ptr as *mut c_void) };
    }

    #[test]
    fn it_frees_memory_sn_realloc() {
        let ptr = unsafe { sn_malloc(8) } as *mut u8;
        let ptr = unsafe { sn_realloc(ptr as *mut c_void, 8) } as *mut u8;
        unsafe { sn_free(ptr as *mut c_void) };
    }
    
    #[test]
    fn it_calculates_malloc_usable_size() {
        let ptr = unsafe { sn_malloc(32) } as *mut u8;
        let usable_size = unsafe { sn_malloc_usable_size(ptr as *mut c_void) };
        assert!(
            usable_size >= 32,
            "usable_size should at least equal to the allocated size"
        );
        unsafe { sn_free(ptr as *mut c_void) };
    }
}