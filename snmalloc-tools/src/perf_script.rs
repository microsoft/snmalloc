//! Minimal parser for the text format emitted by
//! `perf script` (Linux perf-tools).
//!
//! `perf script` is line-oriented and emits one **header line** per
//! sample, followed by zero or more **callstack lines** (one frame
//! each), separated by blank lines.  The canonical header layout
//! looks like this (whitespace condensed):
//!
//! ```text
//! my-app 12345 [001] 1234567.890123: 12345 cache-misses: <ip> <symbol>+<off> (<dso>)
//! my-app 12345 [001] 1234567.890124: 67890 mem_load_retired.l3_miss: <ip> <data_addr> <symbol>+<off> (<dso>)
//!         ffffffff80104000 some_func+0x10 (/path/to/binary)
//!         ffffffff80105000 other_func+0x20 (/path/to/binary)
//! ```
//!
//! For our purposes we only need:
//!
//! - the **instruction pointer** (`ip`) — the address being executed
//!   when the PMU fired, used for branch-miss source-line lookup, and
//! - the **data address** (`data_addr`) — present only for memory-load
//!   events that carry an auxiliary load record (`mem_load_*`,
//!   `mem-loads`, etc.), used for cache-miss attribution against
//!   `lookup_alloc_site`, and
//! - the **callstack frames** (subsequent indented hex addresses), used
//!   for stack-based attribution as a fallback.
//!
//! Everything else (timing, event name, DSO path, symbol+offset) is
//! intentionally discarded.  This keeps the parser small and resilient
//! to perf-version drift — only the leading hex addresses on the
//! callstack lines and the trailing hex tokens on the header line are
//! load-bearing.

use std::fs;
use std::path::Path;

use anyhow::{Context, Result};

/// One parsed `perf script` sample.
///
/// `data_addr` is `None` for PMU events that don't carry a data
/// address (raw `cache-misses`, `branch-misses`, `cycles`, …) and
/// `Some(addr)` for events that do (`mem_load_*`, the various
/// PEBS/IBS load records).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct PerfSample {
    /// Instruction pointer at the moment the PMU fired.  `0` if the
    /// header line had no parseable IP (extremely rare, treated as a
    /// dropped sample by downstream consumers).
    pub ip: u64,
    /// Optional data address for memory-load events.
    pub data_addr: Option<u64>,
    /// Callstack frames captured by `--call-graph`, innermost first.
    /// Empty when `perf record` was invoked without a call-graph mode.
    pub callstack: Vec<u64>,
}

/// Parse the entire contents of a `perf script` text dump into a
/// vector of samples.  Malformed lines are skipped silently — `perf`'s
/// own output occasionally interleaves warnings on stderr that callers
/// have already filtered out, and a single garbled frame should not
/// abort the whole join.
pub fn parse_str(input: &str) -> Vec<PerfSample> {
    let mut out = Vec::new();
    let mut cur: Option<PerfSample> = None;

    for raw in input.lines() {
        let line = raw.trim_end();

        if line.is_empty() {
            // Blank line terminates the current sample.  A subsequent
            // non-empty line will open a fresh one.
            if let Some(s) = cur.take() {
                out.push(s);
            }
            continue;
        }

        // Callstack lines are indented (perf emits a TAB or run of
        // spaces); header lines are not.  Use the leading whitespace
        // as the discriminator.
        let leading_ws = raw.len() - raw.trim_start().len();
        if leading_ws > 0 {
            // Callstack frame: first hex token on the line is the
            // return address.  Some perf versions prefix with `0x`,
            // some don't.
            if let Some(s) = cur.as_mut() {
                if let Some(addr) = first_hex_token(line) {
                    s.callstack.push(addr);
                }
            }
        } else {
            // Header line: flush the previous sample (if any) and
            // start a new one.
            if let Some(s) = cur.take() {
                out.push(s);
            }
            cur = Some(parse_header(line));
        }
    }

    // Flush the trailing sample if the input didn't end with a blank
    // line.  perf normally terminates with a blank line, but be
    // permissive about hand-crafted fixtures.
    if let Some(s) = cur.take() {
        out.push(s);
    }

    out
}

/// Same as [`parse_str`] but reads the bytes from `path`.
pub fn parse_path<P: AsRef<Path>>(path: P) -> Result<Vec<PerfSample>> {
    let path = path.as_ref();
    let text = fs::read_to_string(path)
        .with_context(|| format!("reading perf script output {}", path.display()))?;
    Ok(parse_str(&text))
}

/// Parse a header line into a `PerfSample` with `ip` and (optionally)
/// `data_addr` populated.  The exact column layout varies between
/// perf versions and event types; the reliable invariants are:
///
/// - the line contains a `":"` separating the timestamp from the
///   event payload, and
/// - the payload contains one or more hex tokens; the *first* hex
///   token after the colon is the IP, and (for `mem_load_*`-style
///   events) the *second* hex token is the data address.
///
/// We don't try to interpret the event name — the caller passes the
/// `--filter` flag to `perf script` to restrict the dump to a single
/// event.
fn parse_header(line: &str) -> PerfSample {
    let mut sample = PerfSample::default();
    // Split at the first colon-space (between the timestamp and the
    // event payload).  Older perf versions also emit a colon inside
    // the event name (e.g. `mem_load_retired.l3_miss:pp`), so we use
    // the *last* colon as a more reliable separator.
    let after_colon = match line.rfind(':') {
        Some(idx) => &line[idx + 1..],
        None => line,
    };
    let mut hex_tokens = after_colon.split_whitespace().filter_map(parse_hex);
    if let Some(ip) = hex_tokens.next() {
        sample.ip = ip;
    }
    if let Some(data_addr) = hex_tokens.next() {
        // Only treat the second token as a data address if it looks
        // like one — i.e. it isn't a small offset that just happens
        // to parse as hex.  perf's symbol+offset rendering produces
        // tokens like `+0x10` which `parse_hex` rejects, so any hex
        // value that survives the filter is plausibly an address.
        sample.data_addr = Some(data_addr);
    }
    sample
}

/// Return the first whitespace-separated token of `line` parsed as
/// hex, or `None` if no such token exists.
fn first_hex_token(line: &str) -> Option<u64> {
    line.split_whitespace().find_map(parse_hex)
}

/// Parse a single token as hex.  Accepts both `0xDEADBEEF` and bare
/// `DEADBEEF` forms; rejects tokens that contain non-hex characters
/// (e.g. `some_func+0x10`).  Returns `None` on any failure.
fn parse_hex(tok: &str) -> Option<u64> {
    let stripped = tok.strip_prefix("0x").or_else(|| tok.strip_prefix("0X")).unwrap_or(tok);
    if stripped.is_empty() {
        return None;
    }
    // Reject tokens with embedded `+`/`-` (symbol+offset notation).
    if !stripped.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    u64::from_str_radix(stripped, 16).ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_single_sample_with_callstack() {
        let input = "\
my-app 12345 [001] 1234567.890123: 1 cache-misses: ffffffff80104000 some_func+0x10 (/path/to/binary)
\tffffffff80104000 some_func+0x10 (/path/to/binary)
\tffffffff80105000 other_func+0x20 (/path/to/binary)
";
        let samples = parse_str(input);
        assert_eq!(samples.len(), 1);
        assert_eq!(samples[0].ip, 0xffffffff80104000);
        assert_eq!(samples[0].data_addr, None);
        assert_eq!(
            samples[0].callstack,
            vec![0xffffffff80104000, 0xffffffff80105000]
        );
    }

    #[test]
    fn parses_data_addr_on_mem_load_event() {
        // mem_load_retired-style header: <ip> <data_addr> then symbol.
        let input = "\
my-app 12345 [001] 1234567.890123: 1 mem_load_retired.l3_miss:pp: 0xffffffff80104000 0x00007f1234560000 sym+0x10 (/bin)
";
        let samples = parse_str(input);
        assert_eq!(samples.len(), 1);
        assert_eq!(samples[0].ip, 0xffffffff80104000);
        assert_eq!(samples[0].data_addr, Some(0x00007f1234560000));
    }

    #[test]
    fn blank_line_separates_samples() {
        let input = "\
my-app 1 [0] 0.0: 1 cache-misses: 0xaaa0 sym (/bin)
\t0xaaa0 sym (/bin)

my-app 1 [0] 0.1: 1 cache-misses: 0xbbb0 sym (/bin)
\t0xbbb0 sym (/bin)
";
        let samples = parse_str(input);
        assert_eq!(samples.len(), 2);
        assert_eq!(samples[0].ip, 0xaaa0);
        assert_eq!(samples[1].ip, 0xbbb0);
    }

    #[test]
    fn handles_empty_input() {
        assert!(parse_str("").is_empty());
        assert!(parse_str("\n\n\n").is_empty());
    }

    #[test]
    fn parse_hex_rejects_symbol_offset() {
        assert_eq!(parse_hex("some_func+0x10"), None);
        assert_eq!(parse_hex("0xdeadbeef"), Some(0xdeadbeef));
        assert_eq!(parse_hex("DEADBEEF"), Some(0xdeadbeef));
        assert_eq!(parse_hex(""), None);
        assert_eq!(parse_hex("0x"), None);
    }
}
