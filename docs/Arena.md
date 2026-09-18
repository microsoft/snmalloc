# The Arena: A Bitmap-Indexed Coalescing Range

`Arena` is snmalloc's address-space range that stores free blocks at their
**natural** size — no power-of-two rounding — and serves any request from the
full snmalloc size-class sequence. It sits in the per-thread range pipeline
underneath the slab caches and replaces the historical buddy-based ranges.

This document is the conceptual introduction. For where `Arena` plugs into
the wider range chain, see [`AddressSpace.md`](AddressSpace.md).

## The problem

A buddy allocator only stores power-of-two blocks. A request for 5 chunks
must be served from an 8-chunk buddy block, wasting 3 chunks. We wanted a
range that

* stores blocks at their actual size,
* uses snmalloc's full `(exponent, mantissa)` size-class sequence at the
  range level, and
* still answers "find a block that can serve this request" in O(1).

## The core idea: search upward, mask out exceptions

Free blocks are binned by the *set of size classes they can serve* — the
**servable set**. To allocate, you walk a per-arena non-empty-bins bitmap
upward through the bins; any larger block can be carved down. This almost
works perfectly. The exception is alignment: some bins hold blocks whose
address alignment is too poor to serve certain smaller, *more* aligned size
classes. Those bins must be excluded from the search for those requests.

The implementation builds the per-request filter *positively* as a **serve
mask** — bit `k` set means bin `k` can serve this request — and the lookup
is `find_first_set(bitmap & serve_mask, start_word)`. The serve mask
depends only on the requested size class, not on the block, so it is
precomputed at compile time.

(The original sketch of this design used the equivalent inverse framing of
a "skip mask" with `bitmap & ~skip_mask`; see `arenabins.h` for the
in-tree explanation of why positive is preferred.)

## Why the exceptions exist

snmalloc's size classes follow `S = 2^e + m · 2^(e−B)`, where `B` is the
mantissa-bit width (`INTERMEDIATE_BITS`, 2 in production). Each size class
has a natural alignment `align(S) = S & -S`.

A size class with high alignment needs padding to reach an aligned address
within a block. A block of a *larger* size class with *lower* alignment may
not have room for that padding. Concretely: a block of size 5 at address 1
can serve size 5 (alignment 1) but cannot serve size 4 (alignment 4) —
there is not enough space after padding to the first 4-aligned address.

Same size block, different address, different servable set. This is why
distinct bins per servable-set are needed.

## Bin count grows slowly in B

At each exponent, the distinct servable sets are enumerated exhaustively:

| B | Mantissas/exponent | Bins/exponent | Max mask bits |
|---|-------------------:|--------------:|--------------:|
| 1 | 2                  | 2             | 0             |
| 2 | 4                  | 5             | 1             |
| 3 | 8                  | 13            | 4             |
| 4 | 16                 | 34            | 11            |

Most requests need no exceptions at all. Only size classes whose alignment
exceeds the expected alignment for their position in the sequence have any
bits to mask. The whole structure is constant-folded into a few small tables.

## The two-tree structure

A bitmap alone is not enough — when a bin is non-empty, the arena still has
to *retrieve* and *coalesce* blocks. Each `Arena` therefore maintains:

* **One red-black tree per non-empty bin** (the "bin trees"), keyed by
  block address, giving O(log n) selection within a bin. The non-empty-bins
  bitmap is the index over these trees.

* **One red-black tree of all free blocks** (the "range tree"), keyed by
  address, used to find a block's left/right neighbours for coalescing on
  free.

On allocation: bitmap lookup → choose the bin → pop a block from its
bin tree → `carve` returns pre-pad / aligned request / post-pad → pre and
post (if any) re-enter the arena via the bin and range trees.

On free: range tree lookup → coalesce with neighbours if their tags allow
→ insert the resulting (possibly merged) block.

## Two variants over the same Arena

`Arena` is parameterised by a **Rep** (representation) that decides where
the per-block tree-node state lives. Two reps ship today:

* **`PagemapRep`** — node state lives in the pagemap entry that already
  covers the block. Used by **`LargeArenaRange`**, which manages whole
  chunks and larger. Node access is a pagemap lookup; no in-band space is
  consumed.

* **`InplaceRep`** — node state lives *in the free block itself*, in the
  first units. Used by **`SmallArenaRange`**, which manages sub-chunk
  metadata fragments where no pagemap entry exists for the fragment. The
  layout packs the bin tree pointers, the range tree pointers, and (for
  blocks ≥ 3 units) a large-size word into the leading units of the free
  block. Unit size is `next_pow2(2 · sizeof(CapPtr))` — 16 B without
  CHERI, 32 B with pure-capability CHERI/Morello — large enough to hold
  the two pointers a free block must store.

Both reps drive the same bin / range tree logic in `arena.h`; the bin
classifier and bitmap in `arenabins.h` are shared.

## Why this matters for metadata

Slab metadata typically wants a pow2 client structure (e.g. a 128 B
bitmap) plus a fixed ~32 B header. A buddy-based small range rounds
`160 B → 256 B` (96 B wasted per slab). `SmallArenaRange` rounds to a unit
multiple (`MIN_META_ALIGN`), so the same allocation costs ~160 B. Across
many slabs and large heaps this is real memory.

## Concrete example (B = 2, in-production)

At exponent `e = 2` the size classes are 4, 5, 6, 7, and there are 5 bins,
each labeled by the set of sizes it can serve at this exponent:

    Bin 0: serves {4}
    Bin 1: serves {5}
    Bin 2: serves {4, 5}
    Bin 3: serves {4, 5, 6}
    Bin 4: serves {4, 5, 6, 7}

The per-request serve masks (within this exponent — higher exponents
always serve, so their bits are set):

    Request for 7: serve bins {4}
    Request for 6: serve bins {3, 4}
    Request for 5: serve bins {1, 2, 3, 4}
    Request for 4: serve bins {0, 2, 3, 4}   — bin 1 holds only {5} blocks

Only the size-4 request has an exception: bin 1 must not be picked. All
other requests get the simple "everything at or above" mask.

## Where to look in the code

* `src/snmalloc/backend_helpers/arenabins.h` — bin classification, serve
  masks, the non-empty-bins bitmap, the `carve` primitive.
* `src/snmalloc/backend_helpers/arena.h` — bin-tree-per-bin + range-tree
  structure, allocation and free / coalesce paths.
* `src/snmalloc/backend_helpers/largearenarange.h` — `Arena<PagemapRep>`
  for whole-chunk allocations.
* `src/snmalloc/backend_helpers/smallarenarange.h`,
  `inplacerep.h` — `Arena<InplaceRep>` for sub-chunk metadata.
