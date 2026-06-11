//! Runtime configuration for the snmalloc heap profiler (Phase 4.5).
//!
//! The wrappers in [`crate::profile`] expose the raw FFI surface
//! (`set_sampling_rate` / `sampling_rate` / `snapshot`), but they require
//! the caller to plumb a sampling rate into the allocator by hand after
//! installing it as the global allocator.  In practice we want two
//! ergonomic shortcuts:
//!
//! 1.  A typed, defaulted configuration struct -- [`ProfileConfig`] --
//!     so a binary can describe its desired profiling posture once and
//!     hand it to [`SnMalloc::configure_profiling`] in a single call.
//!
//! 2.  An env-var-driven initializer -- [`SnMalloc::init_profiling_from_env`]
//!     -- so an operator can flip profiling on at the command line
//!     without recompiling.  The two recognised variables are:
//!
//!     - `SNMALLOC_PROFILE_ENABLE`: `1` / `true` / `yes` (case-insensitive)
//!       enables profiling at the default rate (524288 bytes = 512 KiB)
//!       when `SNMALLOC_PROFILE_RATE` is not also set.
//!     - `SNMALLOC_PROFILE_RATE`: a base-10 byte count.  Overrides the
//!       default rate.  Setting this alone is sufficient to enable
//!       profiling -- `_ENABLE` is not required.
//!
//!     Either env var being absent / unparseable / set to a "disable"
//!     value (`0` / `false` / `no` / empty string) leaves the sampling
//!     rate at zero (disabled) unless the other one explicitly enables
//!     it.
//!
//! Both entry points are idempotent and panic-free.  Both are no-ops
//! when the underlying C++ build was compiled with `SNMALLOC_PROFILE`
//! undefined (i.e. the `profiling` Cargo feature is off): the FFI
//! setter is itself a no-op in that case, so [`SnMalloc::sampling_rate`]
//! continues to report `0`.
//!
//! There is **no** `#[ctor]` or static-initializer wiring here.  We
//! deliberately leave the choice of "when to call this" to the embedder
//! -- a constructor that ran before `main` would either need to run
//! after the global allocator is installed (fragile ordering) or would
//! force every consumer of `snmalloc-rs` to pay the env-var lookup cost
//! whether they want profiling or not.  The explicit
//! [`SnMalloc::init_profiling_from_env`] call from `main` (or from a
//! library's first-use path) is both cheaper and easier to reason
//! about.

extern crate std;

use crate::SnMalloc;

/// Default mean sampling interval, in bytes, when
/// `SNMALLOC_PROFILE_ENABLE` is set but `SNMALLOC_PROFILE_RATE` is not.
/// 512 KiB matches the documented "low-overhead, good-coverage"
/// recommendation in `docs/profile-weight.md`.
const DEFAULT_SAMPLING_RATE_BYTES: usize = 524_288;

/// Environment variable that overrides the sampling rate (in bytes).
/// Setting this to a positive integer enables profiling at that rate.
/// Setting it to `0` explicitly disables profiling.  Unparseable values
/// are ignored (treated as "not set").
pub const ENV_PROFILE_RATE: &str = "SNMALLOC_PROFILE_RATE";

/// Environment variable that enables profiling at the default rate
/// when `SNMALLOC_PROFILE_RATE` is unset.  Accepted truthy values
/// (case-insensitive): `1`, `true`, `yes`.  Anything else (including
/// the variable being unset) is treated as "disabled".
pub const ENV_PROFILE_ENABLE: &str = "SNMALLOC_PROFILE_ENABLE";

/// Profiling configuration.  All fields default to "off / disabled".
///
/// Hand this to [`SnMalloc::configure_profiling`] to apply.  Cheap to
/// construct (no allocations) and trivially `Clone` so callers can keep
/// a baseline around and tweak it before re-applying.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ProfileConfig {
    /// Mean sampling interval in bytes.  Zero disables sampling.
    ///
    /// In statistical terms this is the per-byte arrival rate parameter
    /// of the Poisson sampler: setting it to `R` means each byte of
    /// allocation has an independent probability `1 / R` of producing a
    /// sample.  Typical values are 65 536 (high fidelity, ~1.5%
    /// overhead) through 1 048 576 (very low overhead, suitable for
    /// production).
    pub sampling_rate: usize,

    /// If `true`, [`SnMalloc::init_profiling_from_env`] will fall back
    /// to the default sampling rate (512 KiB) when neither
    /// `SNMALLOC_PROFILE_RATE` nor `SNMALLOC_PROFILE_ENABLE` is set in
    /// the environment.  Defaults to `false`: callers must opt in
    /// explicitly either via the struct or via an env var, never by
    /// accident.
    pub enable_from_env: bool,
}

impl ProfileConfig {
    /// Construct a config that sets only the sampling rate.  Equivalent
    /// to `ProfileConfig { sampling_rate, ..Default::default() }`.
    ///
    /// `sampling_rate == 0` is a valid input and disables sampling.
    pub const fn with_sampling_rate(sampling_rate: usize) -> Self {
        Self {
            sampling_rate,
            enable_from_env: false,
        }
    }
}

/// Parse a `SNMALLOC_PROFILE_ENABLE`-style flag from a string.
///
/// Returns `Some(true)` for `1` / `true` / `yes` (case-insensitive),
/// `Some(false)` for `0` / `false` / `no` / empty, and `None` for
/// anything else.  `None` is treated by the callers as "leave the
/// sampling rate unchanged" -- the more conservative default.
fn parse_bool_env(raw: &str) -> Option<bool> {
    // Trim surrounding whitespace so `SNMALLOC_PROFILE_ENABLE=" 1 "`
    // behaves the same as `=1`.  The string fed in by `std::env::var`
    // is already a Rust `String`; the trim is cheap.
    let s = raw.trim();
    match s.to_ascii_lowercase().as_str() {
        "1" | "true" | "yes" => Some(true),
        "0" | "false" | "no" | "" => Some(false),
        _ => None,
    }
}

/// Read the environment and decide on a sampling rate, in bytes.
///
/// Logic, in priority order:
///
/// 1. If `SNMALLOC_PROFILE_RATE` is set to a parseable non-negative
///    integer, use it as-is (including `0`, which explicitly disables).
/// 2. Otherwise, if `SNMALLOC_PROFILE_ENABLE` parses as truthy, use the
///    default rate ([`DEFAULT_SAMPLING_RATE_BYTES`]).
/// 3. Otherwise return `None` -- nothing in the env says "do something",
///    and the caller leaves the sampling rate alone.
///
/// Returning `None` (rather than `Some(0)`) is what lets
/// [`SnMalloc::init_profiling_from_env`] be a true no-op when the
/// environment is empty.  An explicit `SNMALLOC_PROFILE_ENABLE=0`, on
/// the other hand, returns `Some(0)` and disables sampling at the
/// allocator.
fn resolve_rate_from_env() -> Option<usize> {
    // SAFETY (against parallel `set_var` from sibling tests): the
    // resolver is purely read-only; collisions cause us to read a
    // possibly-stale value but never UB.  The integration tests in
    // `tests/profile_runtime_config.rs` serialise the env access with
    // a static mutex specifically because both halves of the contract
    // (set then resolve) need to be atomic w.r.t. each other -- the
    // resolver alone has no such requirement.
    if let Ok(raw) = std::env::var(ENV_PROFILE_RATE) {
        let trimmed = raw.trim();
        if let Ok(parsed) = trimmed.parse::<usize>() {
            return Some(parsed);
        }
        // Unparseable RATE -- fall through to ENABLE.  We could equally
        // well treat this as a hard error and panic, but
        // init_profiling_from_env is documented as panic-free and
        // ignoring garbage matches the conservative end of the dial.
    }
    if let Ok(raw) = std::env::var(ENV_PROFILE_ENABLE) {
        if let Some(true) = parse_bool_env(&raw) {
            return Some(DEFAULT_SAMPLING_RATE_BYTES);
        }
        if let Some(false) = parse_bool_env(&raw) {
            // Explicit "off".  Disable sampling.
            return Some(0);
        }
    }
    None
}

impl SnMalloc {
    /// Apply a [`ProfileConfig`].
    ///
    /// Sets the sampling rate via the FFI getter/setter pair used by
    /// [`SnMalloc::set_sampling_rate`].  Idempotent: calling
    /// `configure_profiling` repeatedly with the same config is
    /// equivalent to calling it once.
    ///
    /// On the feature-off build the FFI setter is a no-op and
    /// [`SnMalloc::sampling_rate`] continues to return `0` regardless
    /// of `cfg.sampling_rate`.  The `enable_from_env` flag is recorded
    /// only for the benefit of [`SnMalloc::init_profiling_from_env`] --
    /// it has no immediate side effect.
    pub fn configure_profiling(&self, cfg: ProfileConfig) {
        self.set_sampling_rate(cfg.sampling_rate);
        // `enable_from_env` deliberately has no immediate effect here:
        // the env-driven default is consulted by `init_profiling_from_env`,
        // which takes its own config.  We expose the field on
        // ProfileConfig so a caller can build one config and reuse it
        // for both `configure_profiling` (immediate apply) and
        // `init_profiling_from_env` (env-driven apply) without two
        // separate types.
        let _ = cfg.enable_from_env;
    }

    /// Read `SNMALLOC_PROFILE_RATE` / `SNMALLOC_PROFILE_ENABLE` from
    /// the process environment and apply the resulting sampling rate
    /// to the allocator.
    ///
    /// Resolution order:
    ///
    /// 1. A parseable integer in `SNMALLOC_PROFILE_RATE` wins, and is
    ///    used verbatim (including `0`, which disables sampling).
    /// 2. Else, a truthy `SNMALLOC_PROFILE_ENABLE` enables sampling at
    ///    the default 512 KiB rate.
    /// 3. Else the call is a no-op -- the sampling rate is unchanged.
    ///
    /// Intended to be called once early in `main`, before any
    /// performance-sensitive code paths run.  Calling it multiple
    /// times is allowed (each call re-reads the environment); but the
    /// configuration is process-global, so there's typically no reason
    /// to do so.
    ///
    /// Returns the rate that was applied, or `None` if the environment
    /// did not request a change.
    pub fn init_profiling_from_env(&self) -> Option<usize> {
        let rate = resolve_rate_from_env()?;
        self.set_sampling_rate(rate);
        Some(rate)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Default config is "everything off".  Catches any future drift
    /// in the `Default` derive (e.g. accidentally promoting a field's
    /// default to a non-zero rate).
    #[test]
    fn default_config_is_off() {
        let cfg = ProfileConfig::default();
        assert_eq!(cfg.sampling_rate, 0);
        assert!(!cfg.enable_from_env);
    }

    /// `with_sampling_rate` is a const-fn helper that only touches the
    /// rate field.  Verifies the other field's default is preserved.
    #[test]
    fn with_sampling_rate_helper() {
        let cfg = ProfileConfig::with_sampling_rate(8192);
        assert_eq!(cfg.sampling_rate, 8192);
        assert!(!cfg.enable_from_env);
    }

    /// `configure_profiling` plumbs `sampling_rate` through to the FFI.
    /// On the feature-on build `sampling_rate()` round-trips it
    /// exactly; on the feature-off build the getter is hard-wired to
    /// `0` and the setter is a no-op.  Restore the saved rate at the
    /// end so sibling tests see the same global state they started
    /// with.
    #[test]
    fn configure_profiling_sets_rate() {
        let a = SnMalloc::new();
        let saved = a.sampling_rate();
        a.configure_profiling(ProfileConfig::with_sampling_rate(8192));
        if cfg!(feature = "profiling") {
            assert_eq!(a.sampling_rate(), 8192);
        } else {
            assert_eq!(a.sampling_rate(), 0);
        }
        a.set_sampling_rate(saved);
        assert_eq!(a.sampling_rate(), saved);
    }

    /// `configure_profiling` with `sampling_rate == 0` disables
    /// sampling.  On the feature-off build this is indistinguishable
    /// from any other input (the rate is always 0); on the feature-on
    /// build it's a real "off" signal.
    #[test]
    fn configure_profiling_zero_disables() {
        let a = SnMalloc::new();
        let saved = a.sampling_rate();
        // First set a non-zero rate so the "back to zero" transition
        // is observable in the feature-on build.
        a.set_sampling_rate(8192);
        a.configure_profiling(ProfileConfig::default());
        assert_eq!(a.sampling_rate(), 0);
        a.set_sampling_rate(saved);
    }

    /// `configure_profiling` is idempotent: applying the same config
    /// twice leaves the rate where one application would.
    #[test]
    fn configure_profiling_is_idempotent() {
        let a = SnMalloc::new();
        let saved = a.sampling_rate();
        let cfg = ProfileConfig::with_sampling_rate(4096);
        a.configure_profiling(cfg.clone());
        let after_once = a.sampling_rate();
        a.configure_profiling(cfg);
        let after_twice = a.sampling_rate();
        assert_eq!(after_once, after_twice);
        a.set_sampling_rate(saved);
    }

    /// `parse_bool_env` accepts the documented truthy / falsy /
    /// unrecognised inputs and is case-insensitive on the alphabetic
    /// values.  Whitespace is trimmed.
    #[test]
    fn parse_bool_env_recognises_documented_inputs() {
        for s in ["1", "true", "TRUE", "True", "yes", "YES", " 1 "] {
            assert_eq!(parse_bool_env(s), Some(true), "input = {s:?}");
        }
        for s in ["0", "false", "FALSE", "no", "NO", "", "  "] {
            assert_eq!(parse_bool_env(s), Some(false), "input = {s:?}");
        }
        for s in ["maybe", "2", "tru", "y"] {
            assert_eq!(parse_bool_env(s), None, "input = {s:?}");
        }
    }
}
