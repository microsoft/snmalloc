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

pub use profile::{BtSample, HeapProfile};

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
