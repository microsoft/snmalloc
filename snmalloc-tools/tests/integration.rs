//! Integration tests for `snmalloc-tools`: exercise each parser /
//! joiner against committed fixture files under `tests/fixtures/`.
//!
//! These tests intentionally avoid spawning the CLI binary; they
//! exercise the library surface directly (`snmalloc_tools::*`) so
//! failures point at the data layer rather than the argv plumbing.

use std::path::PathBuf;

use snmalloc_tools::branch_hints::{BranchHintIndex, HintKind};
use snmalloc_tools::joiner;
use snmalloc_tools::perf_c2c;
use snmalloc_tools::perf_script;

fn fixture(name: &str) -> PathBuf {
    let mut p = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    p.push("tests");
    p.push("fixtures");
    p.push(name);
    p
}

#[test]
fn perf_script_fixture_parses_three_samples() {
    let samples = perf_script::parse_path(fixture("perf_script_sample.txt"))
        .expect("perf_script fixture must parse");
    assert_eq!(samples.len(), 3, "expected three samples in the fixture");

    // Sample 0: branch-misses, IP only, two-frame callstack.
    assert_eq!(samples[0].ip, 0xffffffff80104000);
    assert_eq!(samples[0].data_addr, None);
    assert_eq!(samples[0].callstack.len(), 2);
    assert_eq!(samples[0].callstack[0], 0xffffffff80104000);
    assert_eq!(samples[0].callstack[1], 0xffffffff80105000);

    // Sample 1: cache-misses, IP only, single-frame callstack.
    assert_eq!(samples[1].ip, 0xffffffff80200000);
    assert_eq!(samples[1].data_addr, None);

    // Sample 2: mem_load_retired with a data address — this is the
    // one the cache-miss joiner consumes.
    assert_eq!(samples[2].ip, 0xffffffff80300000);
    assert_eq!(samples[2].data_addr, Some(0x00007fdeadbeef00));
}

#[test]
fn perf_c2c_fixture_parses_two_lines_and_sources() {
    let lines = perf_c2c::parse_path(fixture("perf_c2c_sample.txt"))
        .expect("perf_c2c fixture must parse");
    assert_eq!(lines.len(), 2);
    assert_eq!(lines[0].cacheline_addr, 0xffff8881deadbe00);
    assert_eq!(lines[0].hitm_count, 125);
    assert_eq!(lines[0].srcs.len(), 2);
    assert_eq!(lines[0].srcs[0].pid, 12345);
    assert_eq!(lines[0].srcs[0].ip, 0xffffffff80104000);

    assert_eq!(lines[1].cacheline_addr, 0xffff8881cafef000);
    assert_eq!(lines[1].hitm_count, 80);
    assert_eq!(lines[1].srcs.len(), 1);
}

#[test]
fn branch_hints_fixture_indexes_three_sites() {
    let idx = BranchHintIndex::from_path(fixture("branch_hints_sample.json"))
        .expect("branch hints fixture must parse");
    assert_eq!(idx.len(), 3);
    assert_eq!(
        idx.lookup("src/snmalloc/mem/freelist.h", 412),
        Some(HintKind::Likely)
    );
    assert_eq!(
        idx.lookup("src/snmalloc/mem/corealloc.h", 437),
        Some(HintKind::Unlikely)
    );
    assert_eq!(idx.lookup("does/not/exist.h", 1), None);
}

#[test]
fn cache_miss_joiner_against_unattributed_samples_is_empty() {
    // The fixture's data address is synthetic — it doesn't correspond
    // to any live snmalloc allocation in this test process, so the
    // joiner must produce an empty result (and not panic).  This is
    // the documented "live process only" contract.
    let samples = perf_script::parse_path(fixture("perf_script_sample.txt")).unwrap();
    let rows = joiner::join_cache_misses(&samples, 10).unwrap();
    assert!(rows.is_empty());
}

#[test]
fn c2c_joiner_emits_unattributed_for_synthetic_addrs() {
    // c2c keeps the line in the output (with site_leaf == "<unattributed>")
    // so the operator still sees the HITM count.  Both fixture lines
    // have synthetic addresses, so both must come back unattributed.
    let lines = perf_c2c::parse_path(fixture("perf_c2c_sample.txt")).unwrap();
    let rows = joiner::join_c2c(&lines, 10).unwrap();
    assert_eq!(rows.len(), 2);
    for r in &rows {
        assert_eq!(r.site_leaf, "<unattributed>");
    }
    // Ranked by HITM desc: the 125-HITM line comes first.
    assert_eq!(rows[0].hitm, 125);
    assert_eq!(rows[1].hitm, 80);
}

#[test]
fn cache_miss_joiner_resolves_in_process_allocation() {
    // The live-process attribution path: make a real allocation in
    // this test process, ask the snmalloc-rs profile API to look it
    // up, and feed the resulting pointer back through the joiner as
    // a synthetic perf sample.  This proves the joiner correctly
    // wires together perf data + lookup_alloc_site.
    //
    // We force the sampling rate to 1 byte so every allocation is
    // sampled.  If the profiler is compiled out (`profiling`
    // feature off) the joiner falls through to the empty-result
    // branch, which is the documented degradation; we don't assert
    // success in that case.
    use snmalloc_rs::SnMalloc;

    let alloc = SnMalloc::new();
    if !alloc.profiling_supported() {
        eprintln!(
            "skipping cache_miss_joiner_resolves_in_process_allocation: \
             profiling feature is off in this build"
        );
        return;
    }

    let saved_rate = alloc.sampling_rate();
    alloc.set_sampling_rate(1);

    // A modest live Vec so the sampler captures it.  Hold it past
    // the joiner call so lookup_alloc_site sees it as live.
    let payload: Vec<u8> = vec![0u8; 4096];
    let p = payload.as_ptr();

    // Confirm the in-process API actually resolves this pointer
    // before exercising the joiner — if it doesn't, we'd be testing
    // the joiner's empty-result path again rather than its
    // resolution path.
    if snmalloc_rs::SnMalloc::new().lookup_alloc_site(p).is_none() {
        eprintln!(
            "skipping cache_miss_joiner_resolves_in_process_allocation: \
             allocation was not captured by the sampler (rate=1 may not \
             be honoured in this build)"
        );
        alloc.set_sampling_rate(saved_rate);
        return;
    }

    let synthetic = perf_script::PerfSample {
        ip: 0,
        data_addr: Some(p as u64),
        callstack: vec![],
    };
    let rows = joiner::join_cache_misses(std::slice::from_ref(&synthetic), 10).unwrap();
    // Restore rate before any assert can fail.
    alloc.set_sampling_rate(saved_rate);

    assert_eq!(rows.len(), 1, "expected one attributed row");
    assert_eq!(rows[0].miss_count, 1);

    // Touch payload so the optimizer can't drop the allocation
    // before the lookup.
    std::hint::black_box(payload);
}
