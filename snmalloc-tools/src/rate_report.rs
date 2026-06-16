//! Streaming-mode rate reporter.
//!
//! Reads a line-oriented streaming event log emitted by an application
//! using [`snmalloc_rs::ProfilingSession`] (Phase 5.1 streaming-mode
//! API) and produces a per-site rate report: how many alloc / dealloc
//! events landed at each site, the peak live-bytes high-watermark
//! attributable to that site, and the alloc rate (events per second).
//!
//! ## Why "streaming" vs "snapshot"
//!
//! `SnMalloc::snapshot()` (in `snmalloc-rs`) materialises an in-memory
//! view of allocations that are **currently** sampled-and-live in the
//! process.  That answers "what's holding memory right now?" but
//! systematically under-counts call sites whose allocations are
//! short-lived churn (allocate-and-free inside a request, scratch
//! buffers in a tight loop) -- those allocations are freed before
//! `snapshot()` is called, so they vanish from the live set.
//!
//! Streaming mode records **every** sampled event as it happens
//! (alloc, dealloc, resize), so it captures that transient churn.
//! Feeding the resulting log into this reporter answers a different
//! question: "which call site is the highest-rate allocator?" -- which
//! is what you actually want when optimising a hot path.
//!
//! ## On-disk format
//!
//! The expected log is **JSON Lines (JSONL)**: one JSON object per
//! line, UTF-8.  The reporter accepts a permissive schema (extra
//! fields are ignored) and only the minimum fields are load-bearing:
//!
//! ```jsonl
//! {"ts_ns": 1000000, "kind": "alloc", "site": "0x55a0c0001000", "size": 4096}
//! {"ts_ns": 1001000, "kind": "alloc", "site": "0x55a0c0002000", "size": 256}
//! {"ts_ns": 1002000, "kind": "dealloc", "site": "0x55a0c0001000", "size": 4096}
//! ```
//!
//! Field semantics:
//!
//! - `ts_ns` (u64, optional) -- monotonic-clock timestamp in
//!   nanoseconds.  Used to compute the alloc-rate denominator.  If
//!   any event in the log lacks `ts_ns`, rates fall back to events
//!   divided by 1 second.
//! - `kind` (string, required) -- one of `"alloc"`, `"dealloc"`,
//!   `"resize"`.  Unknown values are skipped (forward-compat).
//! - `site` (string, required) -- the allocation site key.  Typically
//!   the leaf-frame address rendered as `0x` + 16 hex digits (matches
//!   the `site_leaf` field emitted by other snmalloc-tools
//!   subcommands), but any stable string works.
//! - `size` (u64, optional) -- bytes attributable to this event.  For
//!   alloc, the bytes added to the live set; for dealloc, the bytes
//!   removed.  Missing/zero size is treated as 0 (the row is still
//!   counted in alloc/dealloc tallies but doesn't move peak-live).
//!
//! ## Streaming guarantees
//!
//! The reader is strictly stream-based: events are read one line at a
//! time through a buffered reader, and only per-site aggregates are
//! retained in memory.  A 6M-event log uses memory proportional to
//! the number of distinct sites, not the number of events.

use std::collections::HashMap;
use std::fs::File;
use std::io::{self, BufRead, BufReader, Read};
use std::path::Path;

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};

/// One emitted row of the rate report.
///
/// `site` is the raw site key from the log (typically a leaf-frame
/// address rendered as `0x...`); `peak_live_bytes` is the maximum
/// running-sum of `alloc_size - dealloc_size` for that site over the
/// log window.  `alloc_rate_per_sec` is computed as
/// `alloc_count / (last_ts_ns - first_ts_ns) * 1e9`; if the log has
/// fewer than two timestamps or the span is zero, the field is
/// reported as `0.0` rather than NaN/inf.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct RateRow {
    /// Allocation-site key (leaf-frame hex, or whatever the producer
    /// emitted).
    pub site: String,
    /// Number of `kind == "alloc"` events for this site.
    pub alloc_count: u64,
    /// Number of `kind == "dealloc"` events for this site.
    pub dealloc_count: u64,
    /// Peak live-bytes high-watermark: the maximum of the running sum
    /// of alloc bytes minus dealloc bytes observed across the log.
    pub peak_live_bytes: u64,
    /// Alloc events per second, derived from the timestamp span of
    /// the log.  `0.0` if the log lacks usable timestamps.
    pub alloc_rate_per_sec: f64,
}

/// Permissive on-disk record.  Every field is optional except `kind`
/// and `site` (verified during reduction).  Extra fields are ignored.
#[derive(Debug, Deserialize)]
struct RawEvent {
    #[serde(default)]
    ts_ns: Option<u64>,
    kind: Option<String>,
    site: Option<String>,
    #[serde(default)]
    size: Option<u64>,
}

/// Per-site running accumulator.  Owned by the reducer; never escapes
/// the function.  Keeping this off the `RateRow` keeps the public
/// output type narrow and serialisation-friendly.
#[derive(Default)]
struct SiteAcc {
    alloc_count: u64,
    dealloc_count: u64,
    /// Running `alloc_bytes - dealloc_bytes` for this site.  Saturates
    /// at zero on underflow so a log that emits a dealloc before its
    /// matching alloc (e.g. wraparound after a restart) doesn't panic.
    live_bytes: u64,
    /// Watermark of `live_bytes`.
    peak_live_bytes: u64,
}

/// Read a streaming event log file and emit per-site rate rows.
///
/// Streams the file one line at a time -- never loads the whole log
/// into memory.  Per-site aggregates are O(distinct sites), so 6M
/// events touching 1k sites consume O(1k) entries' worth of memory.
///
/// Lines that fail to parse are skipped silently; the reader is
/// resilient to truncated tail records and to the occasional extra
/// blank line.  Returns rows sorted by `alloc_count` descending, then
/// by `site` ascending for deterministic output.
pub fn read_path<P: AsRef<Path>>(path: P) -> Result<Vec<RateRow>> {
    let p = path.as_ref();
    let f = File::open(p)
        .with_context(|| format!("opening streaming event log {}", p.display()))?;
    read_reader(BufReader::new(f))
}

/// Same as [`read_path`] but takes any [`Read`] (used by tests with
/// in-memory fixtures and by callers piping from stdin).
pub fn read_reader<R: Read>(reader: R) -> Result<Vec<RateRow>> {
    let buf = BufReader::new(reader);
    reduce_lines(buf.lines())
}

/// Core stream-reducer: walks an iterator of `io::Result<String>` and
/// folds per-site state.  Pulled out as a standalone function so tests
/// can drive it with in-memory iterators without round-tripping
/// through a `Read`.
fn reduce_lines<I>(lines: I) -> Result<Vec<RateRow>>
where
    I: IntoIterator<Item = io::Result<String>>,
{
    let mut sites: HashMap<String, SiteAcc> = HashMap::new();
    let mut first_ts: Option<u64> = None;
    let mut last_ts: Option<u64> = None;
    let mut any_ts = false;

    for line in lines {
        let line = match line {
            Ok(l) => l,
            // I/O errors during streaming read aren't fatal -- a
            // truncated tail is the common case.  Stop reading; emit
            // what we have.
            Err(_) => break,
        };
        let trimmed = line.trim();
        if trimmed.is_empty() {
            continue;
        }

        let raw: RawEvent = match serde_json::from_str(trimmed) {
            Ok(r) => r,
            // Malformed line: skip and keep going.
            Err(_) => continue,
        };

        let site = match raw.site {
            Some(s) if !s.is_empty() => s,
            _ => continue,
        };
        let kind = match raw.kind.as_deref() {
            Some(k) => k,
            None => continue,
        };
        let size = raw.size.unwrap_or(0);

        if let Some(ts) = raw.ts_ns {
            any_ts = true;
            first_ts = Some(first_ts.map(|f| f.min(ts)).unwrap_or(ts));
            last_ts = Some(last_ts.map(|l| l.max(ts)).unwrap_or(ts));
        }

        let acc = sites.entry(site).or_default();
        match kind {
            "alloc" => {
                acc.alloc_count += 1;
                acc.live_bytes = acc.live_bytes.saturating_add(size);
                if acc.live_bytes > acc.peak_live_bytes {
                    acc.peak_live_bytes = acc.live_bytes;
                }
            }
            "dealloc" => {
                acc.dealloc_count += 1;
                acc.live_bytes = acc.live_bytes.saturating_sub(size);
            }
            // Forward-compat: unknown kinds (incl. "resize") are
            // counted as size-neutral churn -- recorded only as
            // alloc-rate input via timestamp, not as a per-site delta.
            // We deliberately do not bump alloc_count for resize so
            // the rate denominator stays the "fresh-alloc rate", not
            // "fresh-alloc + churn".
            _ => {}
        }
    }

    // Derive seconds spanned by the log.  When the span is zero or
    // unknown, the rate is reported as 0.0 -- producers without
    // timestamps get a clear "no rate" signal rather than infinities.
    let span_sec: f64 = match (first_ts, last_ts, any_ts) {
        (Some(f), Some(l), true) if l > f => (l - f) as f64 / 1_000_000_000.0,
        _ => 0.0,
    };

    let mut rows: Vec<RateRow> = sites
        .into_iter()
        .map(|(site, acc)| {
            let rate = if span_sec > 0.0 {
                acc.alloc_count as f64 / span_sec
            } else {
                0.0
            };
            RateRow {
                site,
                alloc_count: acc.alloc_count,
                dealloc_count: acc.dealloc_count,
                peak_live_bytes: acc.peak_live_bytes,
                alloc_rate_per_sec: rate,
            }
        })
        .collect();

    // Deterministic order: alloc_count desc, then site asc.  Stable
    // ordering matters for CSV/table snapshot tests and for diffing
    // two reports across runs.
    rows.sort_by(|a, b| {
        b.alloc_count
            .cmp(&a.alloc_count)
            .then_with(|| a.site.cmp(&b.site))
    });

    Ok(rows)
}

/// Write rows in CSV format with a header line.  Numeric columns are
/// rendered without thousands separators; the rate column is rendered
/// with six decimal digits (enough resolution for sub-Hz rates).
pub fn write_csv<W: io::Write>(rows: &[RateRow], w: &mut W) -> io::Result<()> {
    writeln!(
        w,
        "site,alloc_count,dealloc_count,peak_live_bytes,alloc_rate_per_sec"
    )?;
    for r in rows {
        writeln!(
            w,
            "{},{},{},{},{:.6}",
            r.site, r.alloc_count, r.dealloc_count, r.peak_live_bytes, r.alloc_rate_per_sec
        )?;
    }
    Ok(())
}

/// Write rows in a fixed-width pretty table (no external crate
/// dependency).  Column widths are constants -- the output is
/// readable in 120-column terminals and stable across runs.
pub fn write_pretty<W: io::Write>(rows: &[RateRow], w: &mut W) -> io::Result<()> {
    writeln!(
        w,
        "{:<20} {:>11} {:>13} {:>16} {:>20}",
        "site", "alloc_count", "dealloc_count", "peak_live_bytes", "alloc_rate_per_sec"
    )?;
    for r in rows {
        writeln!(
            w,
            "{:<20} {:>11} {:>13} {:>16} {:>20.6}",
            r.site, r.alloc_count, r.dealloc_count, r.peak_live_bytes, r.alloc_rate_per_sec
        )?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lines_iter(s: &str) -> Vec<io::Result<String>> {
        s.lines().map(|l| Ok(l.to_string())).collect()
    }

    #[test]
    fn empty_input_yields_no_rows() {
        let rows = reduce_lines(lines_iter("")).unwrap();
        assert!(rows.is_empty());
    }

    #[test]
    fn skips_malformed_and_blank_lines() {
        let log = "\n\
                   not-json\n\
                   {\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":10}\n\
                   {garbled}\n\
                   \n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].site, "0xA");
        assert_eq!(rows[0].alloc_count, 1);
    }

    #[test]
    fn peak_live_tracks_running_max_not_final() {
        let log = "{\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":100,\"ts_ns\":0}\n\
                   {\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":50,\"ts_ns\":1}\n\
                   {\"kind\":\"dealloc\",\"site\":\"0xA\",\"size\":120,\"ts_ns\":2}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows.len(), 1);
        // peak is 150 (after the two allocs), even though final live=30
        assert_eq!(rows[0].peak_live_bytes, 150);
        assert_eq!(rows[0].alloc_count, 2);
        assert_eq!(rows[0].dealloc_count, 1);
    }

    #[test]
    fn rate_uses_timestamp_span() {
        // Two allocs 1 second apart -> rate 2 allocs / 1s = 2.0
        let log = "{\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1,\"ts_ns\":0}\n\
                   {\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1,\"ts_ns\":1000000000}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows.len(), 1);
        assert!((rows[0].alloc_rate_per_sec - 2.0).abs() < 1e-9);
    }

    #[test]
    fn rate_is_zero_when_no_timestamps() {
        let log = "{\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1}\n\
                   {\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].alloc_rate_per_sec, 0.0);
    }

    #[test]
    fn sort_is_alloc_count_desc_then_site_asc() {
        let log = "{\"kind\":\"alloc\",\"site\":\"0xB\",\"size\":1}\n\
                   {\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1}\n\
                   {\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows[0].site, "0xA"); // 2 allocs wins
        assert_eq!(rows[1].site, "0xB");
    }

    #[test]
    fn unknown_kind_is_ignored_for_counts() {
        let log = "{\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":10}\n\
                   {\"kind\":\"resize\",\"site\":\"0xA\",\"size\":99}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows[0].alloc_count, 1);
        // resize does not bump alloc/dealloc tallies and does not
        // affect peak (we only track alloc/dealloc deltas).
        assert_eq!(rows[0].peak_live_bytes, 10);
    }

    #[test]
    fn dealloc_underflow_saturates_at_zero() {
        // Dealloc with no prior alloc -- should not panic.
        let log = "{\"kind\":\"dealloc\",\"site\":\"0xA\",\"size\":999}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        assert_eq!(rows[0].peak_live_bytes, 0);
        assert_eq!(rows[0].dealloc_count, 1);
    }

    #[test]
    fn write_csv_has_header_and_one_row_per_site() {
        let log = "{\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1,\"ts_ns\":0}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        let mut out: Vec<u8> = Vec::new();
        write_csv(&rows, &mut out).unwrap();
        let s = String::from_utf8(out).unwrap();
        assert!(s.starts_with("site,alloc_count,"));
        assert!(s.contains("0xA,1,0,1,"));
    }

    #[test]
    fn write_pretty_emits_aligned_columns() {
        let log = "{\"kind\":\"alloc\",\"site\":\"0xA\",\"size\":1}\n";
        let rows = reduce_lines(lines_iter(log)).unwrap();
        let mut out: Vec<u8> = Vec::new();
        write_pretty(&rows, &mut out).unwrap();
        let s = String::from_utf8(out).unwrap();
        // Header columns separated by whitespace; site column is left-
        // aligned, so the "site" header appears first.
        assert!(s.lines().next().unwrap().starts_with("site"));
        // Data row begins with the site key.
        assert!(s.lines().nth(1).unwrap().starts_with("0xA"));
    }
}
