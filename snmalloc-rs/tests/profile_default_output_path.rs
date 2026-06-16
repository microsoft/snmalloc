//! Tests for `snmalloc_rs::profile::default_output_path` -- the
//! Bazel-aware path-resolution helper introduced in ticket
//! 86aj2dwrr.  The helper inspects three process-global environment
//! variables; we exercise the precedence chain end-to-end here.
//!
//! These tests are gated on the `profiling` Cargo feature because the
//! helper itself only exists in that build configuration.  Without
//! the feature this file compiles down to an empty `tests` binary.
//!
//! All env-var manipulation runs in a single `#[test]` so the
//! save/restore is locally serialised; spawning multiple tests would
//! race against each other on the shared environment.

#![cfg(feature = "profiling")]

use snmalloc_rs::profile::default_output_path;
use std::env;
use std::path::PathBuf;

const ENV_OUT: &str = "SNMALLOC_PROFILE_OUT";
const ENV_BAZEL: &str = "TEST_UNDECLARED_OUTPUTS_DIR";

/// Save the current value of an env var, return a guard that restores
/// it on drop.  This keeps the test idempotent w.r.t. the surrounding
/// process environment -- important because `cargo test` runs all
/// integration tests in a single binary and may set
/// `TEST_UNDECLARED_OUTPUTS_DIR` itself in some CI configurations.
struct EnvGuard {
    key: &'static str,
    prior: Option<String>,
}

impl EnvGuard {
    fn save(key: &'static str) -> Self {
        let prior = env::var(key).ok();
        Self { key, prior }
    }
}

impl Drop for EnvGuard {
    fn drop(&mut self) {
        match &self.prior {
            Some(v) => env::set_var(self.key, v),
            None => env::remove_var(self.key),
        }
    }
}

#[test]
fn precedence_chain_exhaustive() {
    // Save originals so concurrent test binaries / parent env are
    // restored on exit.  EnvGuard restores on drop in reverse
    // declaration order.
    let _g_out = EnvGuard::save(ENV_OUT);
    let _g_bazel = EnvGuard::save(ENV_BAZEL);

    // -------- 1. Explicit override wins -------------------------------
    env::set_var(ENV_OUT, "/tmp/explicit.folded");
    env::set_var(ENV_BAZEL, "/tmp/bazel_should_be_ignored");
    let p = default_output_path();
    assert_eq!(
        p,
        PathBuf::from("/tmp/explicit.folded"),
        "SNMALLOC_PROFILE_OUT must take precedence verbatim"
    );

    // Empty SNMALLOC_PROFILE_OUT is treated as unset so a stray
    // `SNMALLOC_PROFILE_OUT=` in a shell profile doesn't pin us to
    // the current working directory.
    env::set_var(ENV_OUT, "");
    env::set_var(ENV_BAZEL, "/tmp/bazel_outputs");
    let p = default_output_path();
    assert_eq!(
        p,
        PathBuf::from("/tmp/bazel_outputs/heap.folded"),
        "empty SNMALLOC_PROFILE_OUT must fall through to Bazel path"
    );

    // -------- 2. Bazel TEST_UNDECLARED_OUTPUTS_DIR rung ----------------
    env::remove_var(ENV_OUT);
    env::set_var(ENV_BAZEL, "/tmp/bazel_outputs");
    let p = default_output_path();
    assert_eq!(
        p,
        PathBuf::from("/tmp/bazel_outputs/heap.folded"),
        "TEST_UNDECLARED_OUTPUTS_DIR must be suffixed with heap.folded"
    );

    // -------- 3. tmp_dir / pid fallback --------------------------------
    env::remove_var(ENV_OUT);
    env::remove_var(ENV_BAZEL);
    let p = default_output_path();
    // Final rung lives under env::temp_dir() and the file name carries
    // the current PID.  Both invariants matter -- the temp-dir prefix
    // ensures we never accidentally write into the source tree, and
    // the PID stamp prevents concurrent processes from racing on the
    // same path.
    let tmp = env::temp_dir();
    assert!(
        p.starts_with(&tmp),
        "fallback path {p:?} must live under temp_dir {tmp:?}"
    );
    let fname = p
        .file_name()
        .expect("fallback path has a file name")
        .to_str()
        .expect("file name is valid utf-8");
    let expected = format!("heap_{}.folded", std::process::id());
    assert_eq!(fname, expected, "fallback file name must encode the PID");
}
