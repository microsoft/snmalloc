//! `snmalloc-tools` — a library facade over the modules used by the
//! CLI binary in `src/main.rs`.  Exposing them as a library crate
//! lets the integration tests in `tests/integration.rs` exercise the
//! parsers and joiner directly, without re-running the binary.

pub mod branch_hints;
pub mod joiner;
pub mod perf_c2c;
pub mod perf_script;
pub mod rate_report;
