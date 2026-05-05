#!/usr/bin/env python3
"""Merge per-platform llvm-cov JSON exports into a per-line set-union report.

Usage:
  merge_coverage.py --output-json OUT.json --output-md OUT.md \\
      LABEL=PATH [LABEL=PATH ...]

Each PATH is a coverage.json from `llvm-cov export -format=json`. LABEL is
a short platform identifier (e.g. ``linux``, ``macos``, ``selfhost-shim``).

Outputs:
  OUT.json  per-file (executable, covered) line sets, plus per-platform map.
  OUT.md    markdown summary suitable for a PR comment.

The per-line set-union design and its invariant (covered(f) ⊆ executable(f))
are documented inline.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


# --- Schema constants (llvm-cov JSON segment format) -------------------------
# Segment = [line, col, count, has_count, is_region_entry, is_gap_region]
_SEG_LINE = 0
_SEG_COUNT = 2
_SEG_HAS_COUNT = 3
_SEG_IS_GAP = 5
_SEG_MIN_LEN = 6


def normalise_path(path: str) -> str:
    """Normalise a filename so the same source file from different CI runners
    or build trees compares equal.

    Rule: strip everything before (and including) the last ``/src/snmalloc/``
    occurrence, after replacing backslashes with forward slashes. Paths
    without ``/src/snmalloc/`` are kept verbatim (they are out-of-tree).
    """
    p = path.replace("\\", "/")
    needle = "/src/snmalloc/"
    idx = p.rfind(needle)
    if idx >= 0:
        # Result begins with ``src/snmalloc/...`` (no leading slash).
        return p[idx + 1:]
    return p


def parse_platform(doc: dict) -> dict[str, dict]:
    """Convert a single llvm-cov JSON doc into
    ``{normalised_path: {"executable": set[int], "covered": set[int],
                          "regions_executable": int, "regions_covered": int}}``.

    A line ``ℓ`` enters ``executable`` iff at least one segment on ``ℓ``
    has ``has_count == true`` AND ``is_gap_region == false``. It enters
    ``covered`` iff additionally at least one such segment has ``count > 0``.
    By construction ``covered ⊆ executable``.
    """
    files: dict[str, dict] = {}
    for entry in doc.get("data", []):
        for f in entry.get("files", []):
            fn = f.get("filename")
            if not isinstance(fn, str):
                continue
            key = normalise_path(fn)
            executable = files.setdefault(key, {
                "executable": set(),
                "covered": set(),
                "regions_executable": 0,
                "regions_covered": 0,
            })
            for seg in f.get("segments", []):
                if not isinstance(seg, list) or len(seg) < _SEG_MIN_LEN:
                    continue
                if not seg[_SEG_HAS_COUNT]:
                    continue
                if seg[_SEG_IS_GAP]:
                    continue
                line = seg[_SEG_LINE]
                if not isinstance(line, int):
                    continue
                executable["executable"].add(line)
                if isinstance(seg[_SEG_COUNT], (int, float)) and seg[_SEG_COUNT] > 0:
                    executable["covered"].add(line)
            # Region totals: read from per-file summary (advisory only).
            summary = f.get("summary", {})
            regions = summary.get("regions", {})
            r_count = regions.get("count")
            r_covered = regions.get("covered")
            if isinstance(r_count, int):
                executable["regions_executable"] += r_count
            if isinstance(r_covered, int):
                executable["regions_covered"] += r_covered
    return files


def merge(platforms: dict[str, dict[str, dict]]) -> dict:
    """Merge per-platform parsed maps into the canonical merged structure."""
    all_files: set[str] = set()
    for pmap in platforms.values():
        all_files.update(pmap.keys())

    merged_files: dict[str, dict] = {}
    for fn in sorted(all_files):
        executable: set[int] = set()
        covered: set[int] = set()
        for pmap in platforms.values():
            entry = pmap.get(fn)
            if entry is None:
                continue
            executable |= entry["executable"]
            covered |= entry["covered"]
        # Defensive: assert invariant covered ⊆ executable
        assert covered <= executable, f"invariant violation for {fn}"
        merged_files[fn] = {
            "executable": sorted(executable),
            "covered": sorted(covered),
        }

    total_exec = sum(len(v["executable"]) for v in merged_files.values())
    total_covered = sum(len(v["covered"]) for v in merged_files.values())

    plat_out: dict[str, dict] = {}
    for label, pmap in platforms.items():
        files_view = {
            fn: {
                "executable": len(entry["executable"]),
                "covered": len(entry["covered"]),
                "regions": {
                    "executable": entry["regions_executable"],
                    "covered": entry["regions_covered"],
                },
            }
            for fn, entry in pmap.items()
        }
        p_total_exec = sum(v["executable"] for v in files_view.values())
        p_total_covered = sum(v["covered"] for v in files_view.values())
        p_total_r_exec = sum(entry["regions_executable"] for entry in pmap.values())
        p_total_r_covered = sum(entry["regions_covered"] for entry in pmap.values())
        plat_out[label] = {
            "files": files_view,
            "totals": {
                "executable": p_total_exec,
                "covered": p_total_covered,
                "regions": {
                    "executable": p_total_r_exec,
                    "covered": p_total_r_covered,
                },
            },
        }

    return {
        "files": merged_files,
        "totals": {"executable": total_exec, "covered": total_covered},
        "platforms": plat_out,
    }


# --- Markdown rendering ------------------------------------------------------

def md_escape(s: str) -> str:
    """Escape a filename for inclusion in a markdown table cell."""
    return s.replace("|", r"\|").replace("\r", " ").replace("\n", " ")


def _pct(covered: int, executable: int) -> str:
    if executable == 0:
        return "n/a"
    return f"{100.0 * covered / executable:.2f}%"


def _top_dir(path: str) -> str:
    """Group key for the per-directory table.

    For paths under ``src/snmalloc/``, group by the immediate sub-directory
    (e.g. ``src/snmalloc/pal/...`` → ``src/snmalloc/pal``). Other paths are
    grouped under ``other``.
    """
    if path.startswith("src/snmalloc/"):
        rest = path[len("src/snmalloc/"):]
        head, _, _ = rest.partition("/")
        return f"src/snmalloc/{head}" if head else "src/snmalloc"
    return "other"


def _in_scope(path: str) -> bool:
    """Filter a normalised path to ``src/snmalloc/**`` (excluding tests and
    concept headers). Same scoping as ``.copilot/coverage_diff.py``."""
    if not path.startswith("src/snmalloc/"):
        return False
    if path.endswith("_concept.h"):
        return False
    return True


def render_markdown(merged: dict) -> str:
    out: list[str] = []
    # Marker used by .github/workflows/coverage-comment.yml's
    # find-or-create logic (see the dual-marker policy: comment must
    # be authored by github-actions[bot] AND its body must contain
    # this marker). If you change this string you MUST update both
    # occurrences in coverage-comment.yml in lockstep, or comment
    # dedup silently breaks (every run posts a new comment).
    out.append("<!-- snmalloc-coverage-bot -->")
    out.append("## Coverage report (cross-platform merged)")
    out.append("")
    # Headline is in-scope (``src/snmalloc/**``) only — that is the project
    # code being measured. The JSON artifact retains the full unfiltered
    # data for downstream consumers (e.g. ``coverage_diff.py``).
    scoped_exec = 0
    scoped_cov = 0
    for fn, v in merged["files"].items():
        if not _in_scope(fn):
            continue
        scoped_exec += len(v["executable"])
        scoped_cov += len(v["covered"])
    out.append(
        f"**Lines covered (`src/snmalloc/**`): {scoped_cov} / {scoped_exec} "
        f"({_pct(scoped_cov, scoped_exec)})**"
    )
    out.append("")
    out.append(
        "_Merged line coverage is the per-line union across all platforms. "
        "Region coverage is reported per-platform only; no cross-platform "
        "region total is computed._"
    )
    out.append("")

    # Per-directory breakdown (in-scope only).
    dir_totals: dict[str, dict[str, int]] = {}
    for fn, v in merged["files"].items():
        if not _in_scope(fn):
            continue
        d = _top_dir(fn)
        bucket = dir_totals.setdefault(d, {"executable": 0, "covered": 0})
        bucket["executable"] += len(v["executable"])
        bucket["covered"] += len(v["covered"])

    out.append("### Per-directory breakdown")
    out.append("")
    out.append("| Directory | Lines covered | Lines executable | % |")
    out.append("| --- | ---: | ---: | ---: |")
    rows = sorted(
        dir_totals.items(),
        key=lambda kv: (
            (kv[1]["covered"] / kv[1]["executable"]) if kv[1]["executable"] else 1.0,
            kv[0],
        ),
    )
    for d, t in rows:
        out.append(
            f"| {md_escape(d)} | {t['covered']} | {t['executable']} | "
            f"{_pct(t['covered'], t['executable'])} |"
        )
    out.append("")

    # Per-platform contributions (advisory).
    out.append("<details><summary>Per-platform contributions (advisory)</summary>")
    out.append("")
    out.append("| Platform | Lines covered | Lines executable | Lines % | Regions covered | Regions executable | Regions % |")
    out.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: |")
    for label in sorted(merged["platforms"].keys()):
        pt = merged["platforms"][label]["totals"]
        rt = pt["regions"]
        out.append(
            f"| {md_escape(label)} | {pt['covered']} | {pt['executable']} | "
            f"{_pct(pt['covered'], pt['executable'])} | "
            f"{rt['covered']} | {rt['executable']} | "
            f"{_pct(rt['covered'], rt['executable'])} |"
        )
    out.append("")
    out.append("</details>")
    out.append("")
    return "\n".join(out)


# --- CLI ---------------------------------------------------------------------

def parse_inputs(spec_list: list[str]) -> dict[str, Path]:
    inputs: dict[str, Path] = {}
    for spec in spec_list:
        if "=" not in spec:
            raise SystemExit(f"error: input must be LABEL=PATH, got {spec!r}")
        label, _, path = spec.partition("=")
        if not label or not path:
            raise SystemExit(f"error: empty label or path in {spec!r}")
        if label in inputs:
            raise SystemExit(f"error: duplicate label {label!r}")
        inputs[label] = Path(path)
    return inputs


def load_doc(path: Path) -> dict:
    try:
        with path.open() as f:
            doc = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        raise SystemExit(f"error: cannot load {path}: {exc}")
    if not isinstance(doc, dict) or "data" not in doc:
        raise SystemExit(f"error: {path} missing top-level 'data' key")
    return doc


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output-json", required=True, type=Path)
    ap.add_argument("--output-md", required=True, type=Path)
    ap.add_argument("inputs", nargs="+", help="LABEL=PATH pairs")
    args = ap.parse_args(argv)

    spec = parse_inputs(args.inputs)
    platforms: dict[str, dict[str, dict]] = {}
    for label, path in spec.items():
        platforms[label] = parse_platform(load_doc(path))

    merged = merge(platforms)

    args.output_json.write_text(json.dumps(merged, indent=2, sort_keys=True))
    args.output_md.write_text(render_markdown(merged))
    return 0


if __name__ == "__main__":
    sys.exit(main())
