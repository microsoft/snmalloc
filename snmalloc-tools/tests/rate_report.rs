//! Integration tests for the `rate-report` subcommand and its
//! library counterpart in `snmalloc_tools::rate_report`.
//!
//! The fixture under `tests/fixtures/streaming_log_sample.jsonl`
//! exercises the full event matrix: multiple sites, multiple
//! alloc/dealloc events per site, a peak-then-drop pattern, a
//! resize event (size-neutral churn), and explicit timestamps.

use std::path::PathBuf;
use std::process::Command;

use snmalloc_tools::rate_report::{self, RateRow};

fn fixture(name: &str) -> PathBuf {
    let mut p = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    p.push("tests");
    p.push("fixtures");
    p.push(name);
    p
}

fn row_for<'a>(rows: &'a [RateRow], site: &str) -> &'a RateRow {
    rows.iter()
        .find(|r| r.site == site)
        .unwrap_or_else(|| panic!("no row for site {}", site))
}

#[test]
fn streaming_log_fixture_aggregates_per_site() {
    let rows = rate_report::read_path(fixture("streaming_log_sample.jsonl"))
        .expect("rate-report must read fixture");
    assert_eq!(rows.len(), 2, "fixture has two distinct sites");

    let a = row_for(&rows, "0x0000aaaa00000001");
    // Four allocs, two deallocs in the fixture for site A.
    assert_eq!(a.alloc_count, 4);
    assert_eq!(a.dealloc_count, 2);
    // Peak: three back-to-back 1024-byte allocs before the first
    // dealloc = 3072 bytes.  The fourth alloc happens after a dealloc
    // has freed 1024, so it brings live to 3072 again (not higher).
    assert_eq!(a.peak_live_bytes, 3072);

    let b = row_for(&rows, "0x0000bbbb00000002");
    assert_eq!(b.alloc_count, 1);
    assert_eq!(b.dealloc_count, 0);
    assert_eq!(b.peak_live_bytes, 4096);
}

#[test]
fn rate_is_computed_from_timestamp_span() {
    let rows = rate_report::read_path(fixture("streaming_log_sample.jsonl")).unwrap();
    // Fixture spans 1_000_000_000ns (start) to 2_000_000_000ns (end) =
    // 1 second.  Site A has 4 allocs in 1 second -> rate 4.0/s.
    let a = row_for(&rows, "0x0000aaaa00000001");
    assert!(
        (a.alloc_rate_per_sec - 4.0).abs() < 1e-9,
        "expected rate ~4.0, got {}",
        a.alloc_rate_per_sec
    );
}

#[test]
fn rows_are_sorted_by_alloc_count_desc() {
    let rows = rate_report::read_path(fixture("streaming_log_sample.jsonl")).unwrap();
    // Site A has 4 allocs, site B has 1: A must come first.
    assert_eq!(rows[0].site, "0x0000aaaa00000001");
    assert_eq!(rows[1].site, "0x0000bbbb00000002");
}

#[test]
fn write_csv_round_trips_via_serde() {
    let rows = rate_report::read_path(fixture("streaming_log_sample.jsonl")).unwrap();
    let mut out: Vec<u8> = Vec::new();
    rate_report::write_csv(&rows, &mut out).unwrap();
    let s = String::from_utf8(out).unwrap();

    // Header present, two data rows present (site A then site B).
    let lines: Vec<&str> = s.lines().collect();
    assert_eq!(lines.len(), 3);
    assert!(lines[0].starts_with("site,alloc_count,"));
    assert!(lines[1].starts_with("0x0000aaaa00000001,4,2,3072,"));
    assert!(lines[2].starts_with("0x0000bbbb00000002,1,0,4096,"));
}

#[test]
fn cli_rate_report_help_lists_subcommand() {
    // The clap top-level `--help` must mention `rate-report`.  Use the
    // cargo-injected `CARGO_BIN_EXE_snmalloc-tools` path so this works
    // regardless of the workspace layout.
    let exe = env!("CARGO_BIN_EXE_snmalloc-tools");
    let out = Command::new(exe)
        .arg("rate-report")
        .arg("--help")
        .output()
        .expect("failed to spawn snmalloc-tools");
    assert!(out.status.success(), "rate-report --help should succeed");
    let stdout = String::from_utf8_lossy(&out.stdout);
    assert!(
        stdout.contains("--input") && stdout.contains("--pretty"),
        "rate-report --help should mention --input and --pretty (got: {})",
        stdout
    );
}

#[test]
fn cli_rate_report_emits_csv_by_default() {
    let exe = env!("CARGO_BIN_EXE_snmalloc-tools");
    let fixture_path = fixture("streaming_log_sample.jsonl");
    let out = Command::new(exe)
        .arg("rate-report")
        .arg("--input")
        .arg(&fixture_path)
        .output()
        .expect("failed to spawn snmalloc-tools");
    assert!(out.status.success(), "rate-report should exit 0");
    let stdout = String::from_utf8_lossy(&out.stdout);
    // CSV header line is the first thing emitted.
    assert!(
        stdout.starts_with("site,alloc_count,dealloc_count,peak_live_bytes,alloc_rate_per_sec"),
        "expected CSV header, got: {}",
        stdout
    );
    // Both sites appear.
    assert!(stdout.contains("0x0000aaaa00000001"));
    assert!(stdout.contains("0x0000bbbb00000002"));
}

#[test]
fn cli_rate_report_pretty_flag_switches_format() {
    let exe = env!("CARGO_BIN_EXE_snmalloc-tools");
    let fixture_path = fixture("streaming_log_sample.jsonl");
    let out = Command::new(exe)
        .arg("rate-report")
        .arg("--input")
        .arg(&fixture_path)
        .arg("--pretty")
        .output()
        .expect("failed to spawn snmalloc-tools");
    assert!(out.status.success());
    let stdout = String::from_utf8_lossy(&out.stdout);
    // Pretty header is whitespace-separated, not comma-separated.
    assert!(!stdout.contains(','), "pretty output should not contain commas: {}", stdout);
    assert!(stdout.contains("site"));
    assert!(stdout.contains("alloc_count"));
}

#[test]
fn cli_rate_report_top_truncates() {
    let exe = env!("CARGO_BIN_EXE_snmalloc-tools");
    let fixture_path = fixture("streaming_log_sample.jsonl");
    let out = Command::new(exe)
        .arg("rate-report")
        .arg("--input")
        .arg(&fixture_path)
        .arg("--top")
        .arg("1")
        .output()
        .expect("failed to spawn snmalloc-tools");
    assert!(out.status.success());
    let stdout = String::from_utf8_lossy(&out.stdout);
    // Header + 1 data row = 2 lines total.
    let lines: Vec<&str> = stdout.lines().collect();
    assert_eq!(lines.len(), 2, "expected --top 1 to limit to 1 row, got: {}", stdout);
    // Top site by alloc-count is A.
    assert!(lines[1].starts_with("0x0000aaaa00000001"));
}
