//! Phase 4.5 integration tests for [`SnMalloc::init_profiling_from_env`]
//! and [`SnMalloc::configure_profiling`].
//!
//! Manipulating process environment variables is a global side effect.
//! Cargo runs `#[test]`s in this binary in parallel by default, and
//! `profile_accuracy.rs` plus `profile_snapshot.rs` already poke the
//! global sampling rate; we therefore serialise the env-var tests
//! through a local static `Mutex` *and* save/restore both the rate and
//! the env vars themselves.  The mutex is local to this file (each
//! integration test is its own `#[test]` binary in Cargo, so a static
//! `OnceLock<Mutex<()>>` here cannot collide with one in
//! `profile_accuracy.rs`).
//!
//! All assertions are written so they compile and pass in BOTH
//! configurations:
//!
//! - `cargo test`                                  -> profiling feature OFF
//! - `cargo test --features profiling`             -> profiling feature ON
//!
//! With the feature OFF, [`SnMalloc::sampling_rate`] is hard-wired to
//! `0`, so the assertions that the rate matches a non-zero value are
//! skipped (the env-resolution logic still runs and is exercised, but
//! its observable effect at the FFI layer is suppressed by the C-side
//! stub).

use snmalloc_rs::{ProfileConfig, SnMalloc, ENV_PROFILE_ENABLE, ENV_PROFILE_RATE};
use std::env;
use std::sync::{Mutex, MutexGuard, OnceLock};

/// Serialise every test in this file so the env-var manipulations are
/// atomic w.r.t. each other -- and so we never have two tests racing
/// to flip `SNMALLOC_PROFILE_RATE` while a third is reading it.
fn env_lock() -> MutexGuard<'static, ()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

/// Save the current values of the profile-related env vars and the
/// global sampling rate, plus a `Drop`-time restore.
struct EnvGuard {
    saved_rate: usize,
    saved_rate_env: Option<String>,
    saved_enable_env: Option<String>,
}

impl EnvGuard {
    fn new() -> Self {
        let a = SnMalloc::new();
        let g = EnvGuard {
            saved_rate: a.sampling_rate(),
            saved_rate_env: env::var(ENV_PROFILE_RATE).ok(),
            saved_enable_env: env::var(ENV_PROFILE_ENABLE).ok(),
        };
        // Start every test from a known-clean env.  Setting/removing
        // env vars is `unsafe` on the 2024 edition but stable on 2021;
        // this crate is 2021.
        env::remove_var(ENV_PROFILE_RATE);
        env::remove_var(ENV_PROFILE_ENABLE);
        g
    }
}

impl Drop for EnvGuard {
    fn drop(&mut self) {
        // Restore env vars exactly to their pre-test state.
        match &self.saved_rate_env {
            Some(v) => env::set_var(ENV_PROFILE_RATE, v),
            None => env::remove_var(ENV_PROFILE_RATE),
        }
        match &self.saved_enable_env {
            Some(v) => env::set_var(ENV_PROFILE_ENABLE, v),
            None => env::remove_var(ENV_PROFILE_ENABLE),
        }
        // Restore the sampling rate too -- sibling tests in this
        // binary (e.g. the accuracy run in profile_accuracy.rs) also
        // observe this global.
        let a = SnMalloc::new();
        a.set_sampling_rate(self.saved_rate);
    }
}

/// With no env vars set, `init_profiling_from_env` is a no-op: it
/// returns `None` and leaves the sampling rate untouched.
#[test]
fn init_from_env_no_vars_is_noop() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    // Set a known starting rate so we can detect any spurious change.
    a.set_sampling_rate(0);

    let applied = a.init_profiling_from_env();
    assert_eq!(applied, None, "no env vars -> no rate applied");
    assert_eq!(
        a.sampling_rate(),
        0,
        "init_profiling_from_env must not touch the rate when env is empty"
    );
}

/// `SNMALLOC_PROFILE_RATE=4096` resolves to a 4096-byte sampling rate.
/// On the feature-on build the FFI getter reflects it; on the feature-off
/// build the resolver still returns `Some(4096)` but the FFI getter
/// stays at `0` (its hard-wired no-op behaviour).
#[test]
fn init_from_env_rate_only() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    env::set_var(ENV_PROFILE_RATE, "4096");
    let applied = a.init_profiling_from_env();
    assert_eq!(applied, Some(4096), "RATE=4096 should resolve to Some(4096)");
    if cfg!(feature = "profiling") {
        assert_eq!(a.sampling_rate(), 4096);
    } else {
        assert_eq!(a.sampling_rate(), 0);
    }
}

/// `SNMALLOC_PROFILE_ENABLE=0` explicitly disables sampling.
/// Returns `Some(0)` (resolver fired) and the rate is set to 0.
#[test]
fn init_from_env_enable_false() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    // Prime the rate to something non-zero so the disable transition
    // is observable on the feature-on build.
    a.set_sampling_rate(8192);

    env::set_var(ENV_PROFILE_ENABLE, "0");
    let applied = a.init_profiling_from_env();
    assert_eq!(applied, Some(0), "ENABLE=0 should resolve to Some(0)");
    assert_eq!(a.sampling_rate(), 0, "ENABLE=0 must set the rate to 0");
}

/// `SNMALLOC_PROFILE_ENABLE=1` (no RATE) resolves to the default rate
/// of 524288 bytes.  Mirrors the documented "enable at default rate"
/// contract.
#[test]
fn init_from_env_enable_true_uses_default_rate() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    a.set_sampling_rate(0);

    env::set_var(ENV_PROFILE_ENABLE, "1");
    let applied = a.init_profiling_from_env();
    assert_eq!(
        applied,
        Some(524_288),
        "ENABLE=1 with no RATE should resolve to the 512 KiB default"
    );
    if cfg!(feature = "profiling") {
        assert_eq!(a.sampling_rate(), 524_288);
    } else {
        assert_eq!(a.sampling_rate(), 0);
    }
}

/// Truthy aliases for `SNMALLOC_PROFILE_ENABLE` (`true` / `yes`, mixed
/// case, surrounding whitespace) all enable profiling.
#[test]
fn init_from_env_enable_truthy_aliases() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    for v in ["true", "TRUE", "yes", " 1 ", "Yes"] {
        a.set_sampling_rate(0);
        env::remove_var(ENV_PROFILE_RATE);
        env::set_var(ENV_PROFILE_ENABLE, v);
        let applied = a.init_profiling_from_env();
        assert_eq!(
            applied,
            Some(524_288),
            "ENABLE={v:?} should be truthy and resolve to the default rate"
        );
    }
}

/// `SNMALLOC_PROFILE_RATE` takes precedence over
/// `SNMALLOC_PROFILE_ENABLE`.  With both set, the RATE wins (even if
/// ENABLE says "off") -- "set RATE=N explicitly" is the most specific
/// signal we have.
#[test]
fn init_from_env_rate_overrides_enable() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    a.set_sampling_rate(0);
    env::set_var(ENV_PROFILE_RATE, "16384");
    env::set_var(ENV_PROFILE_ENABLE, "0");
    let applied = a.init_profiling_from_env();
    assert_eq!(
        applied,
        Some(16_384),
        "RATE=16384 should override ENABLE=0"
    );
    if cfg!(feature = "profiling") {
        assert_eq!(a.sampling_rate(), 16_384);
    } else {
        assert_eq!(a.sampling_rate(), 0);
    }
}

/// `SNMALLOC_PROFILE_RATE=0` is a valid signal: explicit disable.  It
/// must not fall through to the ENABLE branch.
#[test]
fn init_from_env_rate_zero_disables() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    a.set_sampling_rate(8192);
    env::set_var(ENV_PROFILE_RATE, "0");
    // Set ENABLE=1 too; the RATE=0 should still win.
    env::set_var(ENV_PROFILE_ENABLE, "1");
    let applied = a.init_profiling_from_env();
    assert_eq!(applied, Some(0), "RATE=0 wins, resolves to Some(0)");
    assert_eq!(a.sampling_rate(), 0);
}

/// Unparseable `SNMALLOC_PROFILE_RATE` falls through to the ENABLE
/// branch (instead of panicking).  Documented as "ignore garbage" in
/// the resolver's contract.
#[test]
fn init_from_env_unparseable_rate_falls_through() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    a.set_sampling_rate(0);
    env::set_var(ENV_PROFILE_RATE, "not-a-number");
    env::set_var(ENV_PROFILE_ENABLE, "1");
    let applied = a.init_profiling_from_env();
    assert_eq!(
        applied,
        Some(524_288),
        "garbage RATE should be ignored; ENABLE=1 then drives the default rate"
    );
}

/// `configure_profiling` end-to-end: build a `ProfileConfig`, apply,
/// observe.  On the feature-off build the rate stays at zero.
#[test]
fn configure_profiling_end_to_end() {
    let _lock = env_lock();
    let _guard = EnvGuard::new();
    let a = SnMalloc::new();

    a.configure_profiling(ProfileConfig {
        sampling_rate: 32_768,
        enable_from_env: false,
    });

    if cfg!(feature = "profiling") {
        assert_eq!(a.sampling_rate(), 32_768);
    } else {
        assert_eq!(a.sampling_rate(), 0);
    }

    // Reapply the default (sampling_rate=0) -> sampling disabled.
    a.configure_profiling(ProfileConfig::default());
    assert_eq!(a.sampling_rate(), 0);
}
