//! Phase 9.7 -- runtime tunables.
//!
//! Each tunable is a process-wide singleton.  Cargo runs `#[test]`s
//! within a binary in parallel by default, so two roundtrip tests
//! racing on the same atomic would observe each other's writes and
//! occasionally fail.  We serialise every test in this file through
//! a file-local `Mutex` and save/restore the previous value at each
//! test boundary, matching the pattern in `profile_runtime_config.rs`.
//!
//! These tests are written to pass in every build flavour the
//! `snmalloc-rs` crate supports:
//!
//! - `cargo test`                          (default features)
//! - `cargo test --features stats`         (`FullAllocStats` enabled)
//! - `cargo test --features profiling`     (sampler mirror live)
//!
//! In the `profiling` configuration `snmalloc_set_sample_interval`
//! additionally mirrors into `Sampler::set_sampling_rate`; in the
//! default configuration the sampler is compiled out and the value
//! is stored only.  Either way the public Rust getter must observe
//! the value we just set, which is what the assertions below pin.

use snmalloc_rs::SnMalloc;
use std::sync::{Mutex, MutexGuard, OnceLock};

/// Serialise every test in this file so two roundtrip tests cannot
/// race on the same process-wide atomic.  A poisoned lock here is
/// harmless -- the only thing held across the critical section is
/// our own `Drop` guards.
fn tunable_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

/// RAII restore-on-drop for the three tunables.  Captures the
/// current values in `new()` and writes them back in `drop()` so a
/// panicking test leaves the next test with a pristine baseline.
struct TunableGuard {
    saved_sample_interval: u64,
    saved_decay_rate: u32,
    saved_max_local_cache: u64,
}

impl TunableGuard {
    fn new() -> Self {
        Self {
            saved_sample_interval: SnMalloc::sample_interval(),
            saved_decay_rate: SnMalloc::decay_rate(),
            saved_max_local_cache: SnMalloc::max_local_cache(),
        }
    }
}

impl Drop for TunableGuard {
    fn drop(&mut self) {
        SnMalloc::set_sample_interval(self.saved_sample_interval);
        SnMalloc::set_decay_rate(self.saved_decay_rate);
        SnMalloc::set_max_local_cache(self.saved_max_local_cache);
    }
}

#[test]
fn sample_interval_roundtrip() {
    let _g = tunable_lock();
    let _restore = TunableGuard::new();

    SnMalloc::set_sample_interval(1024);
    assert_eq!(
        SnMalloc::sample_interval(),
        1024,
        "set_sample_interval(1024) must round-trip through \
         sample_interval()"
    );

    // Zero is a meaningful value (disables sampling on the C side).
    SnMalloc::set_sample_interval(0);
    assert_eq!(
        SnMalloc::sample_interval(),
        0,
        "set_sample_interval(0) must round-trip; 0 is a valid \
         'sampling disabled' signal"
    );
}

#[test]
fn decay_rate_roundtrip() {
    let _g = tunable_lock();
    let _restore = TunableGuard::new();

    SnMalloc::set_decay_rate(200);
    assert_eq!(SnMalloc::decay_rate(), 200);

    // 0 ms is a valid value -- once the backend read-side hook
    // lands it will mean "decay immediately".
    SnMalloc::set_decay_rate(0);
    assert_eq!(SnMalloc::decay_rate(), 0);

    // Large value: u32 max minus one to confirm the full range is
    // wired (the C ABI is uint32_t; sanity-check the binding type).
    SnMalloc::set_decay_rate(u32::MAX - 1);
    assert_eq!(SnMalloc::decay_rate(), u32::MAX - 1);
}

#[test]
fn max_local_cache_roundtrip() {
    let _g = tunable_lock();
    let _restore = TunableGuard::new();

    SnMalloc::set_max_local_cache(4 * 1024 * 1024);
    assert_eq!(SnMalloc::max_local_cache(), 4 * 1024 * 1024);

    SnMalloc::set_max_local_cache(0);
    assert_eq!(SnMalloc::max_local_cache(), 0);

    // u64 wide value to confirm we're not silently truncating to
    // size_t on a 32-bit consumer (the C ABI is uint64_t).
    let wide: u64 = 1_u64 << 40;
    SnMalloc::set_max_local_cache(wide);
    assert_eq!(SnMalloc::max_local_cache(), wide);
}

#[test]
fn tunables_are_independent() {
    let _g = tunable_lock();
    let _restore = TunableGuard::new();

    // Set all three to distinguishable values, confirm none of them
    // bleed across.  Catches a swap or aliased-storage bug in either
    // the C ABI shim or the Rust binding.
    SnMalloc::set_sample_interval(0xA1A1_A1A1_A1A1_A1A1);
    SnMalloc::set_decay_rate(0xB2B2_B2B2);
    SnMalloc::set_max_local_cache(0xC3C3_C3C3_C3C3_C3C3);

    assert_eq!(SnMalloc::sample_interval(), 0xA1A1_A1A1_A1A1_A1A1);
    assert_eq!(SnMalloc::decay_rate(), 0xB2B2_B2B2);
    assert_eq!(SnMalloc::max_local_cache(), 0xC3C3_C3C3_C3C3_C3C3);
}

#[test]
fn tunables_survive_thread_spawn() {
    let _g = tunable_lock();
    let _restore = TunableGuard::new();

    // The storage is process-global atomics; a value written from
    // the main thread must be observable from a worker thread, and
    // vice versa.  This pins the "singleton" contract.
    SnMalloc::set_sample_interval(987_654);

    let observed = std::thread::spawn(|| SnMalloc::sample_interval())
        .join()
        .expect("worker thread panicked");

    assert_eq!(
        observed, 987_654,
        "tunable set on main thread must be visible to worker thread \
         (process-wide singleton contract)"
    );

    // And the reverse: worker writes, main reads.
    std::thread::spawn(|| SnMalloc::set_sample_interval(12_345))
        .join()
        .expect("worker thread panicked");
    assert_eq!(SnMalloc::sample_interval(), 12_345);
}

#[test]
fn defaults_are_nonzero() {
    // Pin the contract that the initial values (before any
    // override) are the documented defaults -- non-zero for all
    // three so a binary that never touches the tunables still sees
    // a "useful" configuration.  This guards against an accidental
    // 0-initialised atomic regression in `RuntimeConfig`.
    let _g = tunable_lock();
    let _restore = TunableGuard::new();

    // Force the defaults back into place by reading then writing
    // the saved (pre-test) value, then verify the values are sane.
    // We can't directly assert against `kDefaultSampleIntervalBytes`
    // (it lives in C++); instead we assert the looser "non-zero"
    // contract, which is the actually-load-bearing property for
    // downstream consumers.
    assert!(
        SnMalloc::sample_interval() > 0,
        "default sample interval must be non-zero"
    );
    assert!(
        SnMalloc::decay_rate() > 0,
        "default decay rate must be non-zero"
    );
    assert!(
        SnMalloc::max_local_cache() > 0,
        "default max local cache must be non-zero"
    );
}
