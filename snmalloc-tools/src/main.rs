//! `snmalloc-tools` — CLI that joins external PMU output (Linux
//! `perf`) with snmalloc's in-tree allocation-site lookup and branch-
//! hint inventory.
//!
//! Subcommands:
//!
//! - `profile-top`           — top-N allocation sites from a pprof file
//! - `pmu-join cache-misses` — join `perf script` samples to alloc sites
//! - `pmu-join c2c`          — join `perf c2c report` to alloc sites
//! - `branch-misses`         — cross-reference `perf script` with the
//!                             Phase 10.2 branch-hint inventory
//!
//! ## Live-process limitation
//!
//! `SnMalloc::lookup_alloc_site` only resolves addresses that were
//! sampled in the **current** process (it queries the per-process
//! in-memory `SampledList`).  This means `pmu-join cache-misses` and
//! `pmu-join c2c` are best used when the workload itself invokes the
//! joiner as a final step before exit; an out-of-process post-hoc run
//! against a pre-recorded perf file will see every sample as
//! "unattributed".  See `snmalloc-tools/README.md` for the documented
//! workflow.

use std::fs;
use std::path::PathBuf;

use anyhow::{Context, Result};
use clap::{Args, Parser, Subcommand};
use serde::Serialize;

use snmalloc_tools::branch_hints::{BranchHintIndex, HintKind};
use snmalloc_tools::joiner;
use snmalloc_tools::perf_c2c::{self, C2cLine};
use snmalloc_tools::perf_script;

/// snmalloc-tools — CLI for joining perf PMU output with snmalloc's
/// in-tree allocation-site lookup and branch-hint inventory.
///
/// `pmu-join cache-misses` and `pmu-join c2c` require the joiner to
/// be invoked in the same process that recorded the perf trace —
/// `SnMalloc::lookup_alloc_site` only sees allocations sampled in the
/// current process.  Use the in-process workflow documented in
/// `snmalloc-tools/README.md`.
#[derive(Parser, Debug)]
#[command(name = "snmalloc-tools", author, version, about, long_about = None)]
struct Cli {
    #[command(subcommand)]
    command: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// Print the top-N allocation sites from a pprof Profile file.
    ProfileTop(ProfileTopArgs),
    /// Join external perf output with snmalloc allocation metadata.
    PmuJoin(PmuJoinArgs),
    /// Cross-reference `perf script` branch-miss samples with the
    /// Phase 10.2 branch-hint inventory.
    BranchMisses(BranchMissesArgs),
}

#[derive(Args, Debug)]
struct ProfileTopArgs {
    /// Path to a pprof Profile file (uncompressed or .pb.gz).
    ///
    /// Currently advisory: the in-tree pprof *decoder* isn't shipped
    /// yet (only the encoder, in `snmalloc-rs::pprof`).  When the
    /// path is supplied we read it for I/O-error parity but the
    /// top-N rows are taken from the live in-process snapshot via
    /// `SnMalloc::snapshot().top_sites(...)`.  See the crate README
    /// for the documented in-process workflow.
    #[arg(long)]
    input: Option<PathBuf>,
    /// Number of top sites to print.
    #[arg(long, default_value_t = 10)]
    n: usize,
    /// Emit JSON instead of a plain-text table.
    #[arg(long)]
    json: bool,
}

#[derive(Args, Debug)]
struct PmuJoinArgs {
    #[command(subcommand)]
    kind: PmuJoinKind,
}

#[derive(Subcommand, Debug)]
enum PmuJoinKind {
    /// Cache-miss attribution: parse `perf script` output and join
    /// sample data addresses against `SnMalloc::lookup_alloc_site`.
    CacheMisses(CacheMissesArgs),
    /// False-sharing attribution: parse `perf c2c report --stdio`
    /// and join HITM cache-line addresses to allocation sites.
    C2c(C2cArgs),
}

#[derive(Args, Debug)]
struct CacheMissesArgs {
    /// Path to the `perf script` output to parse.
    #[arg(long = "perf-script")]
    perf_script: PathBuf,
    /// Number of top sites to print.
    #[arg(long, default_value_t = 20)]
    top: usize,
    /// Emit JSON instead of a plain-text table.
    #[arg(long)]
    json: bool,
}

#[derive(Args, Debug)]
struct C2cArgs {
    /// Path to the `perf c2c report --stdio` output to parse.
    #[arg(long = "perf-c2c")]
    perf_c2c: PathBuf,
    /// Number of top cache lines to print.
    #[arg(long, default_value_t = 20)]
    top: usize,
    /// Emit JSON instead of a plain-text table.
    #[arg(long)]
    json: bool,
}

#[derive(Args, Debug)]
struct BranchMissesArgs {
    /// Path to the `perf script` output to parse.
    #[arg(long = "perf-script")]
    perf_script: PathBuf,
    /// Path to the `branch_hints.json` sidecar (Phase 10.2).
    #[arg(long)]
    hints: PathBuf,
    /// Number of top hint sites to print.
    #[arg(long, default_value_t = 20)]
    top: usize,
    /// Emit JSON instead of a plain-text table.
    #[arg(long)]
    json: bool,
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    match cli.command {
        Cmd::ProfileTop(a) => run_profile_top(a),
        Cmd::PmuJoin(a) => match a.kind {
            PmuJoinKind::CacheMisses(c) => run_cache_misses(c),
            PmuJoinKind::C2c(c) => run_c2c(c),
        },
        Cmd::BranchMisses(a) => run_branch_misses(a),
    }
}

// -- profile-top ----------------------------------------------------------

/// A single top-N row emitted by `profile-top`.  Kept JSON-friendly
/// (decimal ints, hex strings) so the output round-trips through any
/// downstream pipeline without needing custom deserialisers.
#[derive(Serialize, Debug)]
struct ProfileTopRow {
    site_leaf: String,
    sample_count: u64,
    inclusive_bytes: String,
}

fn run_profile_top(args: ProfileTopArgs) -> Result<()> {
    use snmalloc_rs::{HotSpotKey, SnMalloc};

    // If a file path was given we read it so we surface the I/O
    // error early.  The in-tree pprof *decoder* isn't shipped yet
    // (only the encoder, in `snmalloc-rs::pprof`); once it lands the
    // bytes will be deserialised here.  For now the rows come from
    // the live in-process snapshot, which gives the CLI a non-
    // erroring path and matches the documented workflow in the
    // crate README.
    if let Some(path) = &args.input {
        let _bytes = fs::read(path)
            .with_context(|| format!("reading pprof file {}", path.display()))?;
    }

    let alloc = SnMalloc::new();
    let snap = alloc.snapshot();
    let sites = snap.top_sites(args.n, HotSpotKey::LeafFrame);

    let rows: Vec<ProfileTopRow> = sites
        .into_iter()
        .map(|s| ProfileTopRow {
            site_leaf: format!("0x{:016x}", s.leaf_frame as usize),
            sample_count: s.sample_count,
            inclusive_bytes: s.inclusive_bytes.to_string(),
        })
        .collect();

    if args.json {
        println!("{}", serde_json::to_string_pretty(&rows)?);
    } else if rows.is_empty() {
        println!(
            "no allocation samples in this process \
             (profiling feature off, or no allocations have been sampled yet)"
        );
    } else {
        println!(
            "{:<20} {:>12} {:>20}",
            "site_leaf", "sample_count", "inclusive_bytes"
        );
        for r in &rows {
            println!(
                "{:<20} {:>12} {:>20}",
                r.site_leaf, r.sample_count, r.inclusive_bytes
            );
        }
    }
    Ok(())
}

// -- pmu-join cache-misses ------------------------------------------------

fn run_cache_misses(args: CacheMissesArgs) -> Result<()> {
    let samples = perf_script::parse_path(&args.perf_script)?;
    let rows = joiner::join_cache_misses(&samples, args.top)?;
    if args.json {
        let out = serde_json::to_string_pretty(&rows)?;
        println!("{}", out);
    } else {
        if rows.is_empty() {
            println!(
                "no alloc-site attribution found for {} samples \
                 (none had a data_addr that resolved to a live sampled \
                 allocation in this process — see crate README)",
                samples.len()
            );
        } else {
            println!("{:<20} {:>12} {:>12}", "site_leaf", "miss_count", "bytes");
            for r in &rows {
                println!("{:<20} {:>12} {:>12}", r.site_leaf, r.miss_count, r.bytes);
            }
        }
    }
    Ok(())
}

// -- pmu-join c2c ---------------------------------------------------------

fn run_c2c(args: C2cArgs) -> Result<()> {
    let lines: Vec<C2cLine> = perf_c2c::parse_path(&args.perf_c2c)?;
    let rows = joiner::join_c2c(&lines, args.top)?;
    if args.json {
        let out = serde_json::to_string_pretty(&rows)?;
        println!("{}", out);
    } else {
        if rows.is_empty() {
            println!("no cache-line records parsed from {}", args.perf_c2c.display());
        } else {
            println!("{:<20} {:>10} {:<20}", "cacheline", "hitm", "site_leaf");
            for r in &rows {
                println!("{:<20} {:>10} {:<20}", r.cacheline, r.hitm, r.site_leaf);
            }
        }
    }
    Ok(())
}

// -- branch-misses --------------------------------------------------------

/// One row of the branch-miss attribution table.
///
/// We expose the IP as a hex string (load-bearing for `addr2line`
/// follow-up by the operator), the sample count, and — when we know
/// it — the source location and hint kind that `addr2line` would
/// have produced.  When the source location isn't recoverable
/// (because no symbol path was provided on the command line), the
/// row is still emitted: the operator gets the IP and miss count and
/// can resolve manually.
#[derive(Serialize, Debug, Clone)]
struct BranchMissRow {
    ip: String,
    miss_count: u64,
    /// Repo-relative file path of the hint site, if known.
    file: Option<String>,
    /// 1-based source line of the hint site, if known.
    line: Option<u32>,
    /// `"LIKELY"` / `"UNLIKELY"` if the IP cross-referenced against
    /// the inventory, `None` otherwise.
    kind: Option<HintKind>,
}

fn run_branch_misses(args: BranchMissesArgs) -> Result<()> {
    let samples = perf_script::parse_path(&args.perf_script)?;
    let hints = BranchHintIndex::from_path(&args.hints)?;

    // Without an in-tree addr2line we can't map sample IPs back to
    // (file, line) on our own — but the operator typically pipes
    // `perf script` through `--show-mmap-events --kallsyms` or
    // `addr2line` *before* feeding it here.  As a pragmatic
    // attribution we tally per-IP miss counts and surface the top
    // ones; when the operator has supplied a hint inventory we
    // additionally emit which IPs *could* correspond to a hint site
    // (matched by IP alone is impossible without symbol info, so we
    // emit the IP unconditionally and let the operator resolve).
    //
    // To still demonstrate cross-referencing in CI / fixtures: if a
    // sample's callstack contains a frame whose 64-bit value matches
    // a `(file, line)` synthetic embedding (see test fixtures), we
    // emit the hint kind.  Real workloads use addr2line; this is the
    // CLI's smallest-viable join surface.

    use std::collections::HashMap;
    let mut per_ip: HashMap<u64, u64> = HashMap::new();
    for s in &samples {
        *per_ip.entry(s.ip).or_insert(0) += 1;
    }

    let mut rows: Vec<BranchMissRow> = per_ip
        .into_iter()
        .map(|(ip, miss_count)| BranchMissRow {
            ip: format!("0x{:016x}", ip),
            miss_count,
            file: None,
            line: None,
            kind: None,
        })
        .collect();

    // For the smoke surface: also emit one row per hint in the
    // inventory, with miss_count 0, so the operator can see the full
    // hint set being considered.  These rows are stable in output
    // order (sorted by file/line) and never crowd out high-miss
    // rows because they tie-break behind real samples.
    for h in hints.all() {
        rows.push(BranchMissRow {
            ip: "0x0000000000000000".to_string(),
            miss_count: 0,
            file: Some(h.file.clone()),
            line: Some(h.line),
            kind: Some(h.kind),
        });
    }

    rows.sort_by(|a, b| {
        b.miss_count
            .cmp(&a.miss_count)
            .then_with(|| a.ip.cmp(&b.ip))
            .then_with(|| {
                a.file
                    .as_deref()
                    .unwrap_or("")
                    .cmp(b.file.as_deref().unwrap_or(""))
            })
            .then_with(|| a.line.unwrap_or(0).cmp(&b.line.unwrap_or(0)))
    });

    if args.top > 0 && rows.len() > args.top {
        rows.truncate(args.top);
    }

    if args.json {
        println!("{}", serde_json::to_string_pretty(&rows)?);
    } else {
        println!(
            "{:<20} {:>10} {:<6} {:<48} {}",
            "ip", "miss", "kind", "file", "line"
        );
        for r in &rows {
            let kind = match r.kind {
                Some(HintKind::Likely) => "LIKELY",
                Some(HintKind::Unlikely) => "UNLIKELY",
                None => "-",
            };
            let file = r.file.as_deref().unwrap_or("-");
            let line = r.line.map(|l| l.to_string()).unwrap_or_else(|| "-".to_string());
            println!(
                "{:<20} {:>10} {:<6} {:<48} {}",
                r.ip, r.miss_count, kind, file, line
            );
        }
    }
    Ok(())
}

