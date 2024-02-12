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

use core::alloc::{GlobalAlloc, Layout};

pub struct SnMalloc;

unsafe impl GlobalAlloc for SnMalloc {
    /// Allocate the memory with the given alignment and size.
    /// On success, it returns a pointer pointing to the required memory address.
    /// On failure, it returns a null pointer.
    /// The client must assure the following things:
    /// - `alignment` is greater than zero
    /// - Other constrains are the same as the rust standard library.
    /// The program may be forced to abort if the constrains are not full-filled.
    #[inline(always)]
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        ffi::sn_rust_alloc(layout.align(), layout.size()) as _
    }

    /// De-allocate the memory at the given address with the given alignment and size.
    /// The client must assure the following things:
    /// - the memory is acquired using the same allocator and the pointer points to the start position.
    /// - Other constrains are the same as the rust standard library.
    /// The program may be forced to abort if the constrains are not full-filled.
    #[inline(always)]
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        ffi::sn_rust_dealloc(ptr as _, layout.align(), layout.size());
    }

    /// Behaves like alloc, but also ensures that the contents are set to zero before being returned.
    #[inline(always)]
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        ffi::sn_rust_alloc_zeroed(layout.align(), layout.size()) as _
    }

    /// Re-allocate the memory at the given address with the given alignment and size.
    /// On success, it returns a pointer pointing to the required memory address.
    /// The memory content within the `new_size` will remains the same as previous.
    /// On failure, it returns a null pointer. In this situation, the previous memory is not returned to the allocator.
    /// The client must assure the following things:
    /// - the memory is acquired using the same allocator and the pointer points to the start position
    /// - `alignment` fulfills all the requirements as `rust_alloc`
    /// - Other constrains are the same as the rust standard library.
    /// The program may be forced to abort if the constrains are not full-filled.
    #[inline(always)]
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        ffi::sn_rust_realloc(ptr as _, layout.align(), layout.size(), new_size) as _
    }
}

impl SnMalloc {
    /// Returns the available bytes in a memory block.
    ///
    /// Note that the value could be higher than the allocation size and
    /// depends very much on the underlying operating system.
    #[inline(always)]
    pub fn usable_size(&self, ptr: *const u8) -> usize {
        unsafe { ffi::sn_malloc_usable_size(ptr as *const _) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
    fn it_usable_size() {
        unsafe {
            let layout = Layout::from_size_align(8, 8).unwrap();
            let alloc = SnMalloc;

            let ptr = alloc.alloc(layout);
            let usz = alloc.usable_size(ptr);
            alloc.dealloc(ptr, layout);
            assert!(usz >= 8);
        }
    }
}
