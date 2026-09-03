//! `memory_stats` reads a process-global atomic counter that every
//! allocation in the process feeds into. Running this assertion in the
//! same binary as other allocating tests (which cargo runs in parallel
//! threads by default) makes the `after <= during` check racy: another
//! test thread can pull a fresh chunk from the backend between the two
//! snapshots and push `current_memory_usage` up. Cargo runs each file
//! under `tests/` as its own binary in its own process, giving us the
//! isolation this test requires.

use snmalloc_rs::SnMalloc;
use std::alloc::{GlobalAlloc, Layout};

#[test]
fn test_memory_stats() {
    let alloc = SnMalloc::new();
    let before = SnMalloc::memory_stats();

    let layout = Layout::from_size_align(1 << 20, 64).unwrap();
    let ptr = unsafe { alloc.alloc(layout) };
    assert!(!ptr.is_null());

    let during = SnMalloc::memory_stats();
    assert!(
        during.current_memory_usage > 0,
        "current usage should be non-zero after allocation"
    );
    assert!(
        during.current_memory_usage >= before.current_memory_usage,
        "current usage should not decrease after allocation"
    );
    assert!(
        during.peak_memory_usage >= before.peak_memory_usage,
        "peak usage should not decrease after allocation"
    );

    unsafe { alloc.dealloc(ptr, layout) };

    let after = SnMalloc::memory_stats();
    assert!(
        after.current_memory_usage <= during.current_memory_usage,
        "current usage should decrease after dealloc"
    );
    assert!(
        after.peak_memory_usage >= during.peak_memory_usage,
        "peak usage should never decrease"
    );
}
