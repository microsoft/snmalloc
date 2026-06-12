//! Loader for the `branch_hints.json` sidecar emitted by Phase 10.2
//! (`scripts/dump_branch_hints.py`).
//!
//! The sidecar is a flat JSON array of `{file, line, kind}` objects;
//! `kind` is either `"LIKELY"` or `"UNLIKELY"` and corresponds to the
//! `SNMALLOC_LIKELY` / `SNMALLOC_UNLIKELY` macro flavours.  See the
//! script's docstring for the canonical schema.

use std::collections::HashMap;
use std::fs;
use std::path::Path;

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};

/// Direction tag emitted by `SNMALLOC_LIKELY` / `SNMALLOC_UNLIKELY`
/// hint sites.  Mirrors the `"kind"` field of the JSON sidecar; the
/// rename attribute keeps the wire format upper-case while the Rust
/// variants stay idiomatic CamelCase.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum HintKind {
    /// `SNMALLOC_LIKELY(...)` — branch predicted taken.
    #[serde(rename = "LIKELY")]
    Likely,
    /// `SNMALLOC_UNLIKELY(...)` — branch predicted not-taken.
    #[serde(rename = "UNLIKELY")]
    Unlikely,
}

/// One row of the branch-hint inventory.
///
/// `file` paths are repo-relative POSIX (e.g.
/// `"src/snmalloc/mem/corealloc.h"`), exactly as the dumper emits
/// them.  `line` is 1-based, matching the macro's source location.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct BranchHint {
    pub file: String,
    pub line: u32,
    pub kind: HintKind,
}

/// In-memory index of the parsed sidecar.
///
/// We keep both the flat list (preserving the source order for
/// deterministic CLI output) and a `(file, line) -> kind` map for
/// O(1) cross-reference against `perf script` source locations.
#[derive(Clone, Debug, Default)]
pub struct BranchHintIndex {
    hints: Vec<BranchHint>,
    by_loc: HashMap<(String, u32), HintKind>,
}

impl BranchHintIndex {
    /// Parse a `branch_hints.json` payload from a raw string.
    ///
    /// Returns an error for malformed JSON or for any entry whose
    /// `kind` field is neither `"LIKELY"` nor `"UNLIKELY"`.  Empty
    /// arrays are accepted and yield an empty index.
    pub fn from_str(s: &str) -> Result<Self> {
        let hints: Vec<BranchHint> = serde_json::from_str(s)
            .context("failed to parse branch_hints.json (expected an array of {file, line, kind})")?;
        Ok(Self::from_vec(hints))
    }

    /// Same as [`Self::from_str`] but reads the bytes from `path`.
    pub fn from_path<P: AsRef<Path>>(path: P) -> Result<Self> {
        let path = path.as_ref();
        let text = fs::read_to_string(path)
            .with_context(|| format!("reading branch hints sidecar {}", path.display()))?;
        Self::from_str(&text)
    }

    fn from_vec(hints: Vec<BranchHint>) -> Self {
        let mut by_loc = HashMap::with_capacity(hints.len());
        for h in &hints {
            by_loc.insert((h.file.clone(), h.line), h.kind);
        }
        Self { hints, by_loc }
    }

    /// All hints in the order they appeared in the sidecar file.
    pub fn all(&self) -> &[BranchHint] {
        &self.hints
    }

    /// Number of hint sites parsed.
    pub fn len(&self) -> usize {
        self.hints.len()
    }

    /// `true` iff no hint sites were loaded.
    pub fn is_empty(&self) -> bool {
        self.hints.is_empty()
    }

    /// Look up a hint by `(file, line)`.  Returns `None` when the
    /// location is not in the inventory (i.e. not an annotated hint
    /// site).  Both repo-relative and absolute paths are accepted at
    /// the caller's discretion — the lookup just compares against the
    /// stored string verbatim, so callers should normalise paths if
    /// they have a choice.
    pub fn lookup(&self, file: &str, line: u32) -> Option<HintKind> {
        self.by_loc.get(&(file.to_string(), line)).copied()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_minimal_array() {
        let s = r#"[
            {"file": "src/snmalloc/mem/freelist.h", "line": 412, "kind": "LIKELY"},
            {"file": "src/snmalloc/mem/corealloc.h", "line": 437, "kind": "UNLIKELY"}
        ]"#;
        let idx = BranchHintIndex::from_str(s).unwrap();
        assert_eq!(idx.len(), 2);
        assert_eq!(
            idx.lookup("src/snmalloc/mem/freelist.h", 412),
            Some(HintKind::Likely)
        );
        assert_eq!(
            idx.lookup("src/snmalloc/mem/corealloc.h", 437),
            Some(HintKind::Unlikely)
        );
        assert_eq!(idx.lookup("nope.h", 1), None);
    }

    #[test]
    fn empty_array_is_ok() {
        let idx = BranchHintIndex::from_str("[]").unwrap();
        assert!(idx.is_empty());
    }

    #[test]
    fn unknown_kind_is_error() {
        let s = r#"[{"file": "x.h", "line": 1, "kind": "MAYBE"}]"#;
        assert!(BranchHintIndex::from_str(s).is_err());
    }

    #[test]
    fn malformed_json_is_error() {
        assert!(BranchHintIndex::from_str("not json").is_err());
    }
}
