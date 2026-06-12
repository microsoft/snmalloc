#!/usr/bin/env python3
"""Dump every SNMALLOC_LIKELY(...) / SNMALLOC_UNLIKELY(...) hint site to JSON.

Used as a build-time sidecar so post-hoc branch-miss analysis (see Phase 10.4,
snmalloc-tools) can map a (file, line) tuple recovered from
perf record/perf script back to a semantic hint kind ("LIKELY" / "UNLIKELY").

Output schema:
    [
      {"file": "src/snmalloc/mem/corealloc.h", "line": 437, "kind": "LIKELY"},
      ...
    ]

Paths are repo-relative (POSIX separators) so the sidecar is portable across
build dirs and platforms. Lines that merely *define* the macros (in
ds_core/defines.h) are skipped so consumers don't have to filter them.

This script intentionally has no third-party dependencies and uses only
stdlib so it can run anywhere CMake's Python interpreter detection succeeds.
A regex over the source tree is enough: snmalloc's hint macros are always
spelled `SNMALLOC_LIKELY(` or `SNMALLOC_UNLIKELY(` (no whitespace before the
paren, no aliases). No clang AST tooling required.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import Iterable

HINT_RE = re.compile(r"\bSNMALLOC_(LIKELY|UNLIKELY)\(")

# Files where the macro is defined, not used as a hint. We skip lines from
# these locations even if they match HINT_RE to keep the inventory free of
# false positives. Paths are repo-relative POSIX.
DEFINITION_FILES: frozenset[str] = frozenset({
    "src/snmalloc/ds_core/defines.h",
})

# File extensions worth scanning. snmalloc is header-mostly C++ but a couple
# of .cc translation units also carry hints (e.g. override/jemalloc_compat.cc).
SOURCE_SUFFIXES: tuple[str, ...] = (".h", ".hh", ".hpp", ".cc", ".cpp", ".cxx")


def iter_source_files(root: Path) -> Iterable[Path]:
    """Yield every C/C++ source file under ``root`` in deterministic order."""
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            yield path


def scan_file(path: Path, repo_root: Path) -> list[dict[str, object]]:
    """Return one entry per hint site in ``path``."""
    rel = path.relative_to(repo_root).as_posix()
    if rel in DEFINITION_FILES:
        return []

    entries: list[dict[str, object]] = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:  # pragma: no cover - unreadable file
        print(f"warning: could not read {path}: {exc}", file=sys.stderr)
        return entries

    for lineno, line in enumerate(text.splitlines(), start=1):
        for match in HINT_RE.finditer(line):
            entries.append({
                "file": rel,
                "line": lineno,
                "kind": match.group(1),
            })
    return entries


def collect(repo_root: Path, source_dir: Path) -> list[dict[str, object]]:
    """Walk ``source_dir`` and return a sorted hint-site inventory."""
    out: list[dict[str, object]] = []
    for path in iter_source_files(source_dir):
        out.extend(scan_file(path, repo_root))
    # Stable order: by file, line, kind. Makes the JSON diff-friendly.
    out.sort(key=lambda e: (e["file"], e["line"], e["kind"]))
    return out


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Emit SNMALLOC_LIKELY / SNMALLOC_UNLIKELY inventory as JSON.",
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=None,
        help="Repository root. Defaults to the parent dir of this script.",
    )
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=None,
        help="Source tree to scan. Defaults to <repo-root>/src/snmalloc.",
    )
    parser.add_argument(
        "-o", "--output",
        type=Path,
        default=None,
        help="Write JSON here. Defaults to stdout.",
    )
    parser.add_argument(
        "--pretty",
        action="store_true",
        help="Pretty-print the JSON (indent=2).",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    repo_root = (
        args.repo_root
        if args.repo_root is not None
        else Path(__file__).resolve().parent.parent
    ).resolve()
    source_dir = (
        args.source_dir
        if args.source_dir is not None
        else repo_root / "src" / "snmalloc"
    ).resolve()

    if not source_dir.is_dir():
        print(
            f"error: source dir does not exist: {source_dir}",
            file=sys.stderr,
        )
        return 1

    entries = collect(repo_root, source_dir)

    if args.pretty:
        payload = json.dumps(entries, indent=2) + "\n"
    else:
        payload = json.dumps(entries, separators=(",", ":"))

    if args.output is None:
        sys.stdout.write(payload)
        if not args.pretty:
            sys.stdout.write("\n")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload, encoding="utf-8")

    # No-op if no hints found: still emit valid JSON ([]) and exit 0, per spec.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
