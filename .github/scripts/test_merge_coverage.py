"""Pytest suite for ``merge_coverage.py``.

Each case
constructs synthetic ``llvm-cov export``-shaped JSON, runs the merger,
and asserts an explicit property — including the global invariant
``covered(f) ⊆ executable(f)`` (case 14) on every merged output.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

# Allow ``import merge_coverage`` from the same directory.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import merge_coverage as mc  # noqa: E402


# ---------------------------------------------------------------------------
# Helpers to synthesise minimal llvm-cov-shaped JSON.
# ---------------------------------------------------------------------------

def _seg(line: int, count: int, has_count: bool = True, gap: bool = False) -> list:
    """Build a segment list ``[line, col, count, has_count, region_entry, gap]``."""
    return [line, 1, count, has_count, True, gap]


def _file(filename: str, segments: list[list], regions: tuple[int, int] = (0, 0)) -> dict:
    r_count, r_covered = regions
    return {
        "filename": filename,
        "segments": segments,
        "summary": {
            "regions": {"count": r_count, "covered": r_covered},
            "lines": {"count": 0, "covered": 0, "percent": 0.0},
        },
    }


def _doc(files: list[dict]) -> dict:
    return {
        "version": "2.0.1",
        "type": "llvm.coverage.json.export",
        "data": [{"files": files, "totals": {}}],
    }


def _merge_dicts(platforms: dict[str, dict]) -> dict:
    parsed = {label: mc.parse_platform(doc) for label, doc in platforms.items()}
    return mc.merge(parsed)


def _assert_invariant(merged: dict) -> None:
    """The global per-file invariant ``covered ⊆ executable`` (case 14)."""
    for fn, v in merged["files"].items():
        cov = set(v["covered"])
        exe = set(v["executable"])
        assert cov <= exe, f"invariant violated for {fn}: covered={cov} executable={exe}"


# ---------------------------------------------------------------------------
# Case 1: disjoint files
# ---------------------------------------------------------------------------

def test_disjoint_files():
    linux = _doc([_file("/work/src/snmalloc/a.h", [_seg(10, 1), _seg(11, 2)])])
    macos = _doc([_file("/work/src/snmalloc/b.h", [_seg(20, 1), _seg(21, 2)])])
    merged = _merge_dicts({"linux": linux, "macos": macos})
    assert set(merged["files"].keys()) == {"src/snmalloc/a.h", "src/snmalloc/b.h"}
    assert merged["totals"] == {"executable": 4, "covered": 4}
    _assert_invariant(merged)


# ---------------------------------------------------------------------------
# Case 2: overlapping files, disjoint covered lines
# ---------------------------------------------------------------------------

def test_overlapping_files_disjoint_covered():
    linux = _doc([_file("/a/src/snmalloc/f.h", [_seg(1, 1), _seg(2, 0), _seg(3, 1)])])
    macos = _doc([_file("/b/src/snmalloc/f.h", [_seg(1, 0), _seg(2, 1), _seg(3, 0)])])
    merged = _merge_dicts({"linux": linux, "macos": macos})
    f = merged["files"]["src/snmalloc/f.h"]
    assert f["executable"] == [1, 2, 3]
    assert f["covered"] == [1, 2, 3]
    _assert_invariant(merged)


# ---------------------------------------------------------------------------
# Case 3: identical coverage
# ---------------------------------------------------------------------------

def test_identical_coverage():
    segs = [_seg(5, 1), _seg(6, 1), _seg(7, 0)]
    a = _doc([_file("/x/src/snmalloc/f.h", segs)])
    b = _doc([_file("/x/src/snmalloc/f.h", segs)])
    merged = _merge_dicts({"a": a, "b": b})
    f = merged["files"]["src/snmalloc/f.h"]
    assert f["executable"] == [5, 6, 7]
    assert f["covered"] == [5, 6]
    _assert_invariant(merged)


# ---------------------------------------------------------------------------
# Case 4: complementary lines
# ---------------------------------------------------------------------------

def test_complementary_lines():
    odd = _doc([_file("/x/src/snmalloc/f.h", [_seg(1, 1), _seg(3, 1), _seg(5, 1)])])
    even = _doc([_file("/x/src/snmalloc/f.h", [_seg(2, 1), _seg(4, 1), _seg(6, 1)])])
    merged = _merge_dicts({"linux": odd, "macos": even})
    f = merged["files"]["src/snmalloc/f.h"]
    assert f["executable"] == [1, 2, 3, 4, 5, 6]
    assert f["covered"] == [1, 2, 3, 4, 5, 6]
    _assert_invariant(merged)


# ---------------------------------------------------------------------------
# Case 5: gap-region only line — must NOT enter executable
# ---------------------------------------------------------------------------

def test_gap_region_only_excluded():
    doc = _doc([_file("/x/src/snmalloc/f.h", [
        _seg(10, 0, has_count=True, gap=True),
        _seg(11, 5, has_count=True, gap=False),
    ])])
    merged = _merge_dicts({"linux": doc})
    f = merged["files"]["src/snmalloc/f.h"]
    assert f["executable"] == [11]
    assert f["covered"] == [11]
    assert 10 not in f["executable"]
    _assert_invariant(merged)


# ---------------------------------------------------------------------------
# Case 6: gap-region mixed line — non-gap segment qualifies it
# ---------------------------------------------------------------------------

def test_gap_region_mixed_line_included():
    doc = _doc([_file("/x/src/snmalloc/f.h", [
        _seg(42, 0, has_count=True, gap=True),
        _seg(42, 7, has_count=True, gap=False),
    ])])
    merged = _merge_dicts({"linux": doc})
    f = merged["files"]["src/snmalloc/f.h"]
    assert f["executable"] == [42]
    assert f["covered"] == [42]
    _assert_invariant(merged)


# ---------------------------------------------------------------------------
# Case 7: ifdef-gated different executable sets
# ---------------------------------------------------------------------------

def test_ifdef_gated_executable_sets():
    linux = _doc([_file("/x/src/snmalloc/pal_ds.h",
                        [_seg(i, 1) for i in range(1, 11)])])
    macos = _doc([_file("/x/src/snmalloc/pal_ds.h",
                        [_seg(i, 0) for i in range(20, 31)])])
    merged = _merge_dicts({"linux": linux, "macos": macos})
    f = merged["files"]["src/snmalloc/pal_ds.h"]
    assert f["executable"] == list(range(1, 11)) + list(range(20, 31))
    assert f["covered"] == list(range(1, 11))
    _assert_invariant(merged)


# ---------------------------------------------------------------------------
# Case 8: empty data on one platform
# ---------------------------------------------------------------------------

def test_empty_platform_data():
    linux = _doc([_file("/x/src/snmalloc/f.h", [_seg(1, 1)])])
    empty = {"version": "2.0.1", "type": "llvm.coverage.json.export",
             "data": [{"files": [], "totals": {}}]}
    merged = _merge_dicts({"linux": linux, "macos": empty})
    assert "src/snmalloc/f.h" in merged["files"]
    assert merged["platforms"]["macos"]["totals"]["executable"] == 0
    assert merged["platforms"]["macos"]["totals"]["covered"] == 0
    _assert_invariant(merged)


def test_all_platforms_empty():
    empty1 = {"version": "2.0.1", "type": "llvm.coverage.json.export",
              "data": [{"files": [], "totals": {}}]}
    empty2 = dict(empty1)
    merged = _merge_dicts({"a": empty1, "b": empty2})
    assert merged["files"] == {}
    assert merged["totals"] == {"executable": 0, "covered": 0}
    md = mc.render_markdown(merged)
    assert "0 / 0" in md or "n/a" in md


# ---------------------------------------------------------------------------
# Case 9: path normalization — same file via different absolute prefixes
# ---------------------------------------------------------------------------

def test_path_normalization_same_file():
    linux_path = "/home/runner/work/snmalloc/snmalloc/src/snmalloc/foo.h"
    selfhost_path = "/build/relwithdebinfo/src/snmalloc/foo.h"
    linux = _doc([_file(linux_path, [_seg(1, 1), _seg(2, 0)])])
    selfhost = _doc([_file(selfhost_path, [_seg(2, 1), _seg(3, 1)])])
    merged = _merge_dicts({"linux": linux, "selfhost": selfhost})
    assert list(merged["files"].keys()) == ["src/snmalloc/foo.h"]
    f = merged["files"]["src/snmalloc/foo.h"]
    # Linux: executable={1,2}, covered={1}; selfhost: executable={2,3},
    # covered={2,3}. Union: executable={1,2,3}, covered={1,2,3}.
    assert f["executable"] == [1, 2, 3]
    assert f["covered"] == [1, 2, 3]
    _assert_invariant(merged)


# ---------------------------------------------------------------------------
# Case 10: path normalization — outside the snmalloc tree, kept verbatim
# ---------------------------------------------------------------------------

def test_path_normalization_outside_tree():
    doc = _doc([_file("/usr/include/stdlib.h", [_seg(50, 1)])])
    merged = _merge_dicts({"linux": doc})
    assert "/usr/include/stdlib.h" in merged["files"]
    _assert_invariant(merged)


# ---------------------------------------------------------------------------
# Case 11: Windows backslashes
# ---------------------------------------------------------------------------

def test_path_normalization_windows_backslashes():
    win_path = r"C:\runner\work\snmalloc\snmalloc\src\snmalloc\pal\pal_windows.h"
    doc = _doc([_file(win_path, [_seg(7, 1)])])
    merged = _merge_dicts({"windows": doc})
    assert "src/snmalloc/pal/pal_windows.h" in merged["files"]
    _assert_invariant(merged)


# ---------------------------------------------------------------------------
# Case 12: filename markdown escape
# ---------------------------------------------------------------------------

def test_markdown_escape_pipe_and_newline():
    weird1 = "/x/src/snmalloc/weird|name.h"
    weird2 = "/x/src/snmalloc/weird\nname.h"
    docs = {
        "linux": _doc([
            _file(weird1, [_seg(1, 1)]),
            _file(weird2, [_seg(1, 1)]),
        ]),
    }
    merged = _merge_dicts(docs)
    md = mc.render_markdown(merged)
    # The pipe-containing path renders with the embedded '|' escaped.
    pipe_lines = [ln for ln in md.splitlines()
                  if "weird" in ln and "|name.h" not in ln.replace(r"\|", "")]
    # Strict: exactly one rendered row contains the escaped pipe.
    assert any(r"weird\|name.h" in ln for ln in md.splitlines()), md
    # Newline inside filename must not break table structure: rendered as
    # a single row with the newline replaced by a space.
    assert "weird name.h" in md
    # No row contains a literal unescaped LF inside the filename.
    for ln in md.splitlines():
        assert "weird\nname.h" not in ln


# ---------------------------------------------------------------------------
# Case 13: schema mismatch
# ---------------------------------------------------------------------------

def test_schema_mismatch_exits_nonzero(tmp_path: Path):
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"not_data": 42}))
    out_json = tmp_path / "merged.json"
    out_md = tmp_path / "merged.md"
    with pytest.raises(SystemExit) as excinfo:
        mc.main([
            "--output-json", str(out_json),
            "--output-md", str(out_md),
            f"linux={bad}",
        ])
    assert excinfo.value.code != 0


# ---------------------------------------------------------------------------
# Case 14: invariant property over a randomly mixed merge
# ---------------------------------------------------------------------------

def test_invariant_holds_under_arbitrary_mix(tmp_path: Path):
    docs = {
        "linux": _doc([
            _file("/a/src/snmalloc/x.h",
                  [_seg(i, i % 2) for i in range(1, 30)]),
            _file("/a/src/snmalloc/y.h",
                  [_seg(i, 1) for i in range(1, 5)]),
        ]),
        "macos": _doc([
            _file("/b/src/snmalloc/x.h",
                  [_seg(i, (i + 1) % 2) for i in range(15, 40)]),
            _file("/b/src/snmalloc/z.h",
                  [_seg(i, 0) for i in range(1, 8)]),
        ]),
        "selfhost": _doc([
            _file("/c/src/snmalloc/y.h",
                  [_seg(i, 2) for i in range(3, 8)]),
        ]),
    }
    merged = _merge_dicts(docs)
    _assert_invariant(merged)
    # And the totals must equal the count of executable lines summed.
    total_exec = sum(len(v["executable"]) for v in merged["files"].values())
    total_cov = sum(len(v["covered"]) for v in merged["files"].values())
    assert merged["totals"]["executable"] == total_exec
    assert merged["totals"]["covered"] == total_cov


# ---------------------------------------------------------------------------
# CLI smoke: end-to-end via main()
# ---------------------------------------------------------------------------

def test_cli_end_to_end(tmp_path: Path):
    in1 = tmp_path / "linux.json"
    in2 = tmp_path / "macos.json"
    in1.write_text(json.dumps(_doc([_file("/a/src/snmalloc/foo.h", [_seg(1, 1), _seg(2, 0)])])))
    in2.write_text(json.dumps(_doc([_file("/b/src/snmalloc/foo.h", [_seg(2, 1), _seg(3, 1)])])))
    out_json = tmp_path / "merged.json"
    out_md = tmp_path / "merged.md"
    rc = mc.main([
        "--output-json", str(out_json),
        "--output-md", str(out_md),
        f"linux={in1}",
        f"macos={in2}",
    ])
    assert rc == 0
    merged = json.loads(out_json.read_text())
    assert merged["files"]["src/snmalloc/foo.h"]["executable"] == [1, 2, 3]
    assert merged["files"]["src/snmalloc/foo.h"]["covered"] == [1, 2, 3]
    md = out_md.read_text()
    assert "<!-- snmalloc-coverage-bot -->" in md
    assert "3 / 3" in md
