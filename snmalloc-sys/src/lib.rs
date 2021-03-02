#![no_std]
#![allow(non_camel_case_types)]

use core::ffi::c_void;
use libc::size_t;

extern "C" {
    /// Allocate the memory with the given alignment and size. 
    /// On success, it returns a pointer pointing to the required memory address. 
    /// On failure, it returns a null pointer.
    /// The client must assure the following things:
    /// - `alignment` is greater than zero
    /// - `alignment` is a power of 2
    /// The program may be forced to abort if the constrains are not full-filled.
    pub fn rust_alloc(alignment: size_t, size: size_t) -> *mut c_void;

    /// De-allocate the memory at the given address with the given alignment and size. 
    /// The client must assure the following things:
    /// - the memory is acquired using the same allocator and the pointer points to the start position.
    /// - `alignment` and `size` is the same as allocation
    /// The program may be forced to abort if the constrains are not full-filled.
    pub fn rust_dealloc(ptr: *mut c_void, alignment: size_t, size: size_t) -> c_void;

    /// Re-allocate the memory at the given address with the given alignment and size. 
    /// On success, it returns a pointer pointing to the required memory address. 
    /// The memory content within the `new_size` will remains the same as previous.
    /// On failure, it returns a null pointer. In this situation, the previous memory is not returned to the allocator.
    /// The client must assure the following things:
    /// - the memory is acquired using the same allocator and the pointer points to the start position
    /// - `alignment` and `old_size` is the same as allocation
    /// - `alignment` fulfills all the requirements as `rust_alloc`
    /// The program may be forced to abort if the constrains are not full-filled.
    pub fn rust_realloc(ptr: *mut c_void, alignment: size_t, old_size: size_t, new_size: size_t) -> *mut c_void;
    
    /// Allocate `count` items of `size` length each.
    ///
    /// Returns `null` if `count * size` overflows or on out-of-memory.
    ///
    /// All items are initialized to zero.
    pub fn sn_calloc(count: usize, size: usize) -> *mut c_void;
    
    /// Allocate `size` bytes.
    ///
    /// Returns pointer to the allocated memory or null if out of memory.
    /// Returns a unique pointer if called with `size` 0.
    pub fn sn_malloc(size: usize) -> *mut c_void;

    /// Re-allocate memory to `newsize` bytes.
    ///
    /// Return pointer to the allocated memory or null if out of memory. If null
    /// is returned, the pointer `p` is not freed. Otherwise the original
    /// pointer is either freed or returned as the reallocated result (in case
    /// it fits in-place with the new size).
    ///
    /// If `p` is null, it behaves as [`mi_malloc`]. If `newsize` is larger than
    /// the original `size` allocated for `p`, the bytes after `size` are
    /// uninitialized.
    pub fn sn_realloc(p: *mut c_void, newsize: usize) -> *mut c_void;

    /// Free previously allocated memory.
    ///
    /// The pointer `p` must have been allocated before (or be null).
    pub fn sn_free(p: *mut c_void);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn it_frees_memory_malloc() {
        let ptr = unsafe { rust_alloc(8, 8) } as *mut u8;
        unsafe {*ptr = 127; assert_eq!(*ptr, 127)};
        unsafe { rust_dealloc(ptr as *mut c_void, 8, 8) };
    }
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
    fn it_reallocs_correctly() {
        let mut ptr = unsafe { rust_alloc(8, 8) } as *mut u8;
        unsafe {*ptr = 127; assert_eq!(*ptr, 127)};
        ptr = unsafe { rust_realloc(ptr as *mut c_void, 8, 8, 16) } as *mut u8;
        unsafe {assert_eq!(*ptr, 127)};
        unsafe { rust_dealloc(ptr as *mut c_void, 8, 16) };
    }
}
