//! Glue between the parsers and snmalloc's in-tree
//! [`SnMalloc::lookup_alloc_site`] (Phase 10.1).
//!
//! The joiner walks a vector of parsed [`PerfSample`]s, tries to map
//! each sample's data address back to the allocation that owns it,
//! and tallies a per-allocation-site miss count.  Samples whose data
//! address falls outside any live sampled allocation are routed into
//! a single "unattributed" bucket — they're still useful as a
//! denominator for the attribution rate, but they don't have a
//! site-level home.
//!
//! ## Live-process limitation
//!
//! `lookup_alloc_site` is backed by the per-process in-memory
//! `SampledList`; it only resolves addresses that were sampled in the
//! **current** process.  In the `snmalloc-tools` CLI this means the
//! cache-miss / c2c subcommands are only useful when the same binary
//! that recorded the perf trace also runs the joiner — typically the
//! workload itself, with the tool invoked as a post-run cleanup step
//! before exit.  See the crate-level README for the documented
//! workflow; integration tests in `tests/integration.rs` exercise the
//! joiner against allocations made by the test process itself.

use anyhow::Result;
use serde::Serialize;
use snmalloc_rs::SnMalloc;

use crate::perf_c2c::C2cLine;
use crate::perf_script::PerfSample;

/// One row of the cache-miss attribution table.
///
/// `site_leaf` is the innermost (leaf) frame of the allocation's
/// recorded call stack — the most precise "who allocated this byte"
/// signal we have without symbolication.  `bytes` is the allocation's
/// rounded size (matches the `allocated_size` field on `BtSample`).
#[derive(Clone, Debug, Default, Serialize)]
pub struct CacheMissRow {
    /// Innermost frame address of the allocation site, rendered as a
    /// hex string so JSON / table output is portable.
    pub site_leaf: String,
    /// Total miss-event count attributed to this site.
    pub miss_count: u64,
    /// Allocation size in bytes (sizeclass-rounded).
    pub bytes: u64,
}

/// One row of the c2c (false-sharing) attribution table.
#[derive(Clone, Debug, Default, Serialize)]
pub struct C2cRow {
    /// Cache-line virtual address, rendered as hex.
    pub cacheline: String,
    /// Total HITM count for the line.
    pub hitm: u64,
    /// Innermost frame of the allocation that owns the line (hex), or
    /// `"<unattributed>"` if the line didn't map to any live sampled
    /// allocation in the current process.
    pub site_leaf: String,
}

/// Run the cache-miss join.  For each sample with a `data_addr`,
/// invoke [`SnMalloc::lookup_alloc_site`]; tally hits by the leaf
/// frame of the returned allocation stack.  Returns the top `n`
/// sites by miss count, ranked descending.
pub fn join_cache_misses(samples: &[PerfSample], n: usize) -> Result<Vec<CacheMissRow>> {
    let alloc = SnMalloc::new();
    // (leaf_addr_as_usize, allocated_size) -> miss_count
    let mut buckets: std::collections::HashMap<(usize, u64), u64> = std::collections::HashMap::new();

    for s in samples {
        let Some(da) = s.data_addr else { continue };
        let Some(frames) = alloc.lookup_alloc_site(da as *const u8) else {
            continue;
        };
        let leaf = frames
            .frames
            .first()
            .copied()
            .map(|p| p as usize)
            .unwrap_or(0);
        let bytes = frames.allocated_size as u64;
        let entry = buckets.entry((leaf, bytes)).or_insert(0);
        *entry += 1;
    }

    // Materialise to rows, sort by miss_count desc, then by leaf asc
    // for determinism.
    let mut rows: Vec<CacheMissRow> = buckets
        .into_iter()
        .map(|((leaf, bytes), miss_count)| CacheMissRow {
            site_leaf: format!("0x{:016x}", leaf),
            miss_count,
            bytes,
        })
        .collect();
    rows.sort_by(|a, b| {
        b.miss_count
            .cmp(&a.miss_count)
            .then_with(|| a.site_leaf.cmp(&b.site_leaf))
    });
    if n > 0 && rows.len() > n {
        rows.truncate(n);
    }
    Ok(rows)
}

/// Run the c2c (false-sharing) join.  For each cache-line summary
/// row, try to resolve the line's address to an allocation site and
/// emit a row.  Lines that don't resolve are emitted with a sentinel
/// site so the operator still sees the HITM count.
pub fn join_c2c(lines: &[C2cLine], n: usize) -> Result<Vec<C2cRow>> {
    let alloc = SnMalloc::new();
    let mut rows: Vec<C2cRow> = lines
        .iter()
        .map(|l| {
            let site_leaf = match alloc.lookup_alloc_site(l.cacheline_addr as *const u8) {
                Some(frames) => {
                    let leaf = frames
                        .frames
                        .first()
                        .copied()
                        .map(|p| p as usize)
                        .unwrap_or(0);
                    format!("0x{:016x}", leaf)
                }
                None => "<unattributed>".to_string(),
            };
            C2cRow {
                cacheline: format!("0x{:016x}", l.cacheline_addr),
                hitm: l.hitm_count,
                site_leaf,
            }
        })
        .collect();

    rows.sort_by(|a, b| {
        b.hitm
            .cmp(&a.hitm)
            .then_with(|| a.cacheline.cmp(&b.cacheline))
    });
    if n > 0 && rows.len() > n {
        rows.truncate(n);
    }
    Ok(rows)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn join_cache_misses_empty_input() {
        let rows = join_cache_misses(&[], 10).unwrap();
        assert!(rows.is_empty());
    }

    #[test]
    fn join_cache_misses_skips_samples_without_data_addr() {
        // Sample with no data_addr is silently dropped, never panics.
        let samples = vec![PerfSample {
            ip: 0xdeadbeef,
            data_addr: None,
            callstack: vec![0xdeadbeef],
        }];
        let rows = join_cache_misses(&samples, 10).unwrap();
        assert!(rows.is_empty());
    }

    #[test]
    fn join_c2c_unattributed_is_emitted() {
        // Cache lines that don't resolve to a live sampled alloc
        // still appear in the output with the sentinel site.  This
        // is the documented behaviour: the operator wants to see the
        // HITM count even when attribution fails.
        let lines = vec![C2cLine {
            cacheline_addr: 0xdead_beef_0000,
            hitm_count: 42,
            srcs: vec![],
        }];
        let rows = join_c2c(&lines, 10).unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].hitm, 42);
        assert_eq!(rows[0].site_leaf, "<unattributed>");
        assert_eq!(rows[0].cacheline, "0x0000dead_beef_0000".replace('_', ""));
    }

    #[test]
    fn join_c2c_ranks_by_hitm_desc() {
        let lines = vec![
            C2cLine { cacheline_addr: 0x1000, hitm_count: 5, srcs: vec![] },
            C2cLine { cacheline_addr: 0x2000, hitm_count: 50, srcs: vec![] },
            C2cLine { cacheline_addr: 0x3000, hitm_count: 1, srcs: vec![] },
        ];
        let rows = join_c2c(&lines, 10).unwrap();
        assert_eq!(rows.len(), 3);
        assert_eq!(rows[0].hitm, 50);
        assert_eq!(rows[1].hitm, 5);
        assert_eq!(rows[2].hitm, 1);
    }
}
