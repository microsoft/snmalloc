//! Minimal parser for `perf c2c report --stdio` output.
//!
//! `perf c2c` ("cache-to-cache") reports HITM events — loads that
//! were served from a *modified* line in another core's cache — and
//! groups them by cache line.  The `--stdio` rendering is a series
//! of human-readable tables; the one we need is the
//! **"Shared Data Cache Line Table"**, which has one row per
//! contended line.
//!
//! Each row in that table starts with an index/record number, then a
//! batch of integer columns (HITM count, local/remote breakdown,
//! load counts), then a hexadecimal cache-line virtual address, then
//! the producing/consuming code-location strings.  The exact column
//! count varies between perf releases; the reliable invariants are:
//!
//! - the row's first whitespace-separated token is a record index
//!   that parses as decimal,
//! - the *last* `0x`-prefixed hexadecimal token on the line is the
//!   cache-line virtual address, and
//! - at least one of the integer columns before the address is the
//!   total HITM count (we use the largest integer column on the row,
//!   which empirically lines up with the "Tot Hitm" field across the
//!   perf versions we've sampled).
//!
//! Sources lines (the per-cacheline detail rows that follow each
//! cache-line summary row) carry the consumer-side IPs and PIDs:
//!
//! ```text
//!    -------- Pid 12345 cpu  0 ...  ip 0xffffffff80104000  ...
//! ```
//!
//! We extract `(ip, pid)` tuples from those lines and attach them to
//! the most recently parsed cache-line record.  Lines that don't
//! match either shape are ignored.

use std::fs;
use std::path::Path;

use anyhow::{Context, Result};

/// One row of the Shared Data Cache Line Table.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct C2cLine {
    /// Virtual address of the contended cache line.
    pub cacheline_addr: u64,
    /// Total HITM count attributed to this line.
    pub hitm_count: u64,
    /// Per-source instruction-pointer / PID tuples extracted from the
    /// detail rows that follow the line's summary row.
    pub srcs: Vec<C2cSource>,
}

/// One consumer-side source attached to a [`C2cLine`].
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct C2cSource {
    pub ip: u64,
    pub pid: u32,
}

/// Parse the full text of a `perf c2c report --stdio` dump.  Malformed
/// rows are skipped; an entirely unrecognised file yields an empty
/// vector rather than an error so callers can degrade gracefully.
pub fn parse_str(input: &str) -> Vec<C2cLine> {
    let mut out: Vec<C2cLine> = Vec::new();
    let mut in_table = false;

    for raw in input.lines() {
        let line = raw.trim_end();

        // The Shared Data Cache Line Table is preceded by a header
        // banner that contains the phrase "Shared Data Cache Line"
        // (case-sensitive in every perf release we've seen).  Use
        // that as the gate so we don't try to parse stray hex tokens
        // from unrelated sections (the Load Latency table also has
        // hex addresses, but we don't want them).
        if !in_table {
            if line.contains("Shared Data Cache Line") {
                in_table = true;
            }
            continue;
        }

        // A blank line by itself doesn't end the table — perf emits
        // spacer rows inside the rendering.  Pure banner rules
        // (`===`) inside the table are *also* ignored: they appear
        // both immediately after the section title and as decorative
        // separators between sub-tables.  We stop the table only on
        // the next "Table" or "Report" header that comes with
        // text, never on a pure rule.
        let trimmed = line.trim_start();
        if trimmed.contains("Table")
            && !trimmed.contains("Shared Data Cache Line")
            && !trimmed.starts_with('=')
            && !trimmed.starts_with('#')
        {
            in_table = false;
            continue;
        }

        // Skip dividers (`----`), column headers, and decorative rows.
        if trimmed.starts_with('#') || trimmed.starts_with('-') || trimmed.starts_with('=') {
            // Detail rows in some perf versions are prefixed with
            // `--------`; treat those as sources rather than dividers
            // if they contain a `Pid` and `ip` substring.
            if trimmed.contains("Pid ") && trimmed.contains("ip ") {
                if let Some(last) = out.last_mut() {
                    if let Some(src) = parse_source_line(trimmed) {
                        last.srcs.push(src);
                    }
                }
            }
            continue;
        }

        if trimmed.is_empty() {
            continue;
        }

        // Try a summary row first (has a trailing 0x... cacheline
        // address).  If that fails, try a source row.
        if let Some(record) = parse_summary_row(trimmed) {
            out.push(record);
        } else if let Some(src) = parse_source_line(trimmed) {
            if let Some(last) = out.last_mut() {
                last.srcs.push(src);
            }
        }
    }

    out
}

/// Read and parse `path`.
pub fn parse_path<P: AsRef<Path>>(path: P) -> Result<Vec<C2cLine>> {
    let path = path.as_ref();
    let text = fs::read_to_string(path)
        .with_context(|| format!("reading perf c2c report {}", path.display()))?;
    Ok(parse_str(&text))
}

/// Parse one summary row of the Shared Data Cache Line Table.
///
/// A summary row looks roughly like:
///
/// ```text
///   0     0    125     22    103     0     0    0xffff8881deadbe00 [...]
/// ```
///
/// Returns `None` if the row doesn't contain a `0x...` hex token,
/// which is the cheapest sentinel for "this isn't a summary row".
fn parse_summary_row(line: &str) -> Option<C2cLine> {
    // Find the last 0x-prefixed token; that's the cacheline addr.
    let cacheline_addr = line
        .split_whitespace()
        .rev()
        .find_map(parse_hex_prefixed)?;

    // Collect every decimal integer column that appears *before* the
    // address.  The HITM count is the largest such integer in every
    // perf release we sampled — empirically the Tot Hitm column
    // dominates the smaller per-source breakdown columns.  Using
    // "largest" rather than a positional index keeps the parser
    // tolerant of perf-version drift in column ordering.
    let mut max_int: u64 = 0;
    for tok in line.split_whitespace() {
        if tok.starts_with("0x") || tok.starts_with("0X") {
            // Stop once we hit the cacheline address; the symbol/dso
            // tokens after it can contain digits we don't want to
            // count.
            break;
        }
        if let Ok(n) = tok.parse::<u64>() {
            if n > max_int {
                max_int = n;
            }
        }
    }

    Some(C2cLine {
        cacheline_addr,
        hitm_count: max_int,
        srcs: Vec::new(),
    })
}

/// Parse one detail row.  Detail rows carry `Pid <N>` and `ip 0x...`
/// (or `ip: 0x...`) substrings somewhere on the line.
fn parse_source_line(line: &str) -> Option<C2cSource> {
    let pid = find_after_keyword(line, "Pid")?;
    let pid: u32 = pid.parse().ok()?;
    let ip_tok = find_after_keyword(line, "ip")?;
    let ip = parse_hex_prefixed(ip_tok).or_else(|| parse_hex_bare(ip_tok))?;
    Some(C2cSource { ip, pid })
}

/// Find the whitespace-separated token immediately after `kw`.
/// Tolerates a trailing colon on the keyword (`Pid:`, `ip:`).
fn find_after_keyword<'a>(line: &'a str, kw: &str) -> Option<&'a str> {
    let mut it = line.split_whitespace().peekable();
    while let Some(tok) = it.next() {
        let stripped = tok.trim_end_matches(':');
        if stripped == kw {
            if let Some(next) = it.next() {
                return Some(next.trim_end_matches(','));
            }
        }
    }
    None
}

fn parse_hex_prefixed(tok: &str) -> Option<u64> {
    let s = tok.strip_prefix("0x").or_else(|| tok.strip_prefix("0X"))?;
    if s.is_empty() || !s.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    u64::from_str_radix(s, 16).ok()
}

fn parse_hex_bare(tok: &str) -> Option<u64> {
    if tok.is_empty() || !tok.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    u64::from_str_radix(tok, 16).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_summary_and_sources() {
        let input = "\
=================================================
                Shared Data Cache Line Table
=================================================
#       Total      Tot  --------- Cacheline ----------
#      Hitm     Hitm    Address                Node
#
       125      125    0xffff8881deadbe00      0
        -------- Pid 12345 cpu 0 ip 0xffffffff80104000 ...
        -------- Pid 12345 cpu 1 ip 0xffffffff80105000 ...
        80       80    0xffff8881cafef000      0
        -------- Pid 67890 cpu 2 ip 0xffffffff80106000 ...
";
        let lines = parse_str(input);
        assert_eq!(lines.len(), 2);
        assert_eq!(lines[0].cacheline_addr, 0xffff8881deadbe00);
        assert_eq!(lines[0].hitm_count, 125);
        assert_eq!(lines[0].srcs.len(), 2);
        assert_eq!(lines[0].srcs[0].ip, 0xffffffff80104000);
        assert_eq!(lines[0].srcs[0].pid, 12345);

        assert_eq!(lines[1].cacheline_addr, 0xffff8881cafef000);
        assert_eq!(lines[1].hitm_count, 80);
        assert_eq!(lines[1].srcs.len(), 1);
        assert_eq!(lines[1].srcs[0].ip, 0xffffffff80106000);
        assert_eq!(lines[1].srcs[0].pid, 67890);
    }

    #[test]
    fn empty_input_yields_empty() {
        assert!(parse_str("").is_empty());
    }

    #[test]
    fn ignores_input_without_table_banner() {
        // No "Shared Data Cache Line" banner -> nothing parsed even
        // if there are hex tokens floating around.
        let input = "some random output\n  100 200 0xdeadbeef\n";
        assert!(parse_str(input).is_empty());
    }
}
