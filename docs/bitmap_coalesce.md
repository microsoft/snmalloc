# Bitmap-Indexed Coalescing Range

## Background

### The buddy system and its generalisation

The classic binary buddy allocator manages memory as power-of-two blocks.
A block of size $2^k$ must be aligned to a $2^k$ boundary.  This alignment
invariant makes splitting and merging trivial — a block's *buddy* is at a
known address — but wastes memory: the only valid sizes are exact powers
of two, so a 33-byte request consumes a 64-byte block.

A natural generalisation keeps the alignment discipline but admits a richer
set of size classes.  Instead of only powers of two, we allow sizes with a
few mantissa bits:

$$S = 2^e + m \cdot 2^{e - B}$$

where $e \ge 0$ is the exponent and $m \in \{0, \ldots, 2^B - 1\}$ is
the mantissa, controlled by a parameter $B$ (the number of *intermediate
bits*).  When $e < B$ the step size $2^{e-B}$ is fractional, so only those
$(e, m)$ combinations that yield an integer are valid size classes.  With
$B = 0$ this is the classic buddy system.  With $B = 2$ the valid sizes
(in chunk units) are:

| $e$ | valid $m$ | sizes |
|-----|-----------|-------|
| 0   | 0         | 1     |
| 1   | 0, 2      | 2, 3  |
| 2   | 0, 1, 2, 3 | 4, 5, 6, 7 |
| 3   | 0, 1, 2, 3 | 8, 10, 12, 14 |
| …   | all        | … |

For $e \ge B$ every mantissa value $m$ yields an integer, giving $2^B$
sub-classes per power-of-two doubling.  The full sequence is

$$1,\;2,\;3,\;4,\;5,\;6,\;7,\;8,\;10,\;12,\;14,\;16,\;20,\;24,\;28,\;32,\;\ldots$$

This halves internal fragmentation compared to binary buddy while
preserving the structural properties that make buddy systems efficient.

### Natural alignment

Define the *natural alignment* of a positive integer $S$ as the largest
power of two dividing $S$:

$$\operatorname{align}(S) = S \mathbin{\&} (\mathord{\sim}(S - 1))$$

For a power-of-two size this equals the size itself (the classic buddy
constraint); for $S = 12$ it is 4; for $S = 96$ it is 32.

When computing alignment for an *address* rather than a size, the same
formula is used with one exception: address 0 is divisible by every power
of two, so $\operatorname{align}(0)$ should be treated as arbitrarily
large (i.e. no alignment constraint at all).

### The alignment invariant

The generalised buddy system preserves the classical buddy's alignment
discipline: every block of size $S$ at address $A$ must satisfy

$$A \bmod \operatorname{align}(S) = 0$$

This invariant is important for allocation: when a free list stores blocks
of size class $S$, any block popped from the list is guaranteed to satisfy
the alignment requirement.  No address checking is needed.

### Maximum size class at a given alignment

A critical property of the size-class scheme: for a given address alignment
$\alpha = 2^k$, there is a bounded maximum valid size class.  Only size
classes whose natural alignment is $\le \alpha$ may be placed at that
address.

With $B$ intermediate bits, valid sizes at exponent level $e$ are
$2^e + m \cdot 2^{e-B}$ for $m \in \{0,\ldots,2^B-1\}$.  The largest
size at a given $\alpha$ is obtained at $e = \log_2(\alpha) + B$,
$m = 2^B - 1$:

$$S_{\max} = (2^{B+1} - 1) \cdot \alpha$$

With $B = 0$ (classic buddy): $S_{\max} = \alpha$.
With $B = 2$: $S_{\max} = 7\alpha$, so the maximum block at alignment 16 is
$7 \times 16 = 112$.

## The problem: efficient aligned allocation

When a block of size $S$ with alignment $A = \operatorname{align}(S)$ is
requested, the allocator must find a free block that contains a
naturally-aligned region of at least $S$ bytes.

Three classic approaches and their tradeoffs:

**Binary buddy** stores blocks of exact power-of-two size at
power-of-two-aligned addresses.  Allocation is O(1) — pop any block from
the appropriate bin.  But internal fragmentation is up to 50%.

**TLSF** stores blocks at their actual size with no alignment invariant.
Low internal fragmentation, O(1) coalescing.  But aligned allocation
requires scanning the free list to find a block whose address satisfies
the alignment — O(n) per bin in the worst case.

**Bitmap coalescing** (this design) stores blocks at their actual
(maximally-coalesced) size.  A flat bitmap indexes blocks by the set of
size classes they can serve, considering all possible alignment offsets.
Allocation is O(1) via bitmap scan.  Coalescing is simple boundary-tag
merging with no decomposition.

## Illustrated walkthrough

All sizes are in abstract *chunks* (the minimum allocation unit,
$2^{14}$ bytes in production).  With $B = 2$ the valid size classes
are $1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, \ldots$

### Setup: a 32-chunk arena

```
Address: 0       4       8      12      16      20      24      28      32
         ├───────┼───────┼───────┼───────┼───────┼───────┼───────┼───────┤
         │                     free (32)                                 │
         └───────────────────────────────────────────────────────────────┘
```

### Step 1: Allocate 5, 10, 5, 7 chunks

**Binary buddy** rounds each request up to a power of two:

| Request | Buddy gives | Waste |
|---------|-------------|-------|
| 5       | 8           | 3     |
| 10      | 16          | 6     |
| 5       | 8           | 3     |
| 7       | 8           | 1     |

Total waste: 10 chunks (31%).

```
Binary buddy after allocations:
 0               8              24              32
 ├───────────────┼──────────────────────────────┼───────────────┤
 │ alloc 8 (5+3W)│      alloc 16 (10+6W)       │ alloc 8 (7+1W)│
 └───────────────┴──────────────────────────────┴───────────────┘
```

**Bitmap coalescing** uses the exact size classes with zero waste:

```
Bitmap coalesce after allocations:
 0         5        15        20          27    32
 ├─────────┼────────────────┼───────────┼───────┼─────┤
 │ alloc 5 │   alloc 10     │  alloc 5  │alloc 7│free5│
 └─────────┴────────────────┴───────────┴───────┴─────┘
```

### Step 2: Free the 10-chunk block at address 5

The block $[5, 15)$ is returned.  Adjacent blocks on both sides are still
allocated, so no coalescing occurs — a single 10-chunk free block is
inserted.

```
After freeing [5,15):
 0         5        15        20          27    32
 ├─────────┼────────────────┼───────────┼───────┼─────┤
 │ alloc 5 │   FREE 10      │  alloc 5  │alloc 7│free5│
 └─────────┴────────────────┴───────────┴───────┴─────┘
```

### Step 3: Free the 5-chunk block at address 15

Now $[15, 20)$ is freed next to the free block $[5, 15)$.  The contiguous
free range is $[5, 20)$ — 15 chunks.

**Binary buddy** decomposes into 6 blocks:

```
Binary buddy free decomposition of [5, 20):
 5  6   8          12   14  16          20
 ├──┼───┼──────────┼────┼───┼──────────┤
 │1 │ 2 │    4     │  2 │ 1 │    4     │
 └──┴───┴──────────┴────┴───┴──────────┘
 6 free blocks, largest = 4
```

A subsequent request for 10 chunks would *fail* despite 15 contiguous
free chunks — buddy's largest block is only 4.

**Bitmap coalescing** maximally coalesces: the left walk finds
$[5, 15)$ adjacent, merges into a single 15-chunk block.

```
Bitmap coalesce after merging [5, 20):
 5                              20
 ├──────────────────────────────┤
 │       FREE 15                │
 └──────────────────────────────┘
```

One free block of size 15.  A request for 10 chunks succeeds immediately.

### Step 4: Allocate 6 chunks with 4-chunk alignment

The caller needs a 6-chunk block at an address divisible by 4.

**Binary buddy**: largest block is 4.  *Allocation fails.*

**TLSF**: has the 15-chunk block at address 5.  Address 5 is not
4-aligned; TLSF must scan the free list to find a block where a 4-aligned
6-chunk sub-range exists — O(n) in general.

**Bitmap coalescing**: the 15-chunk block at address 5 was binned into a
slot that guarantees it can serve this request.  The bitmap search finds it
in O(1).  The range wrapper carves out the aligned subrange:

```
Carving [8,14) from [5,20), returning remainders:
 5     8        14               20
 ├─────┼────────┼────────────────┤
 │free3│alloc 6 │   free 6       │
 └─────┴────────┴────────────────┘
```

No scanning required — the bitmap guarantees the search result can serve
the requested size class at a naturally-aligned offset.

### Summary comparison

```
                 ┌─────────────┬──────────────┬───────────────────┐
                 │ Binary Buddy│    TLSF      │ Bitmap Coalesce   │
 ┌───────────────┼─────────────┼──────────────┼───────────────────┤
 │ Size classes  │ powers of 2 │ fine-grained │ fine-grained      │
 │ Internal frag.│ up to 50%   │ ~3%          │ ~3%               │
 │ Coalesce cost │ O(log n)    │ O(1)         │ O(neighbours)     │
 │ Aligned search│ free (nat.) │ O(n) scan    │ O(1) bitmap       │
 │ Free path     │ split+merge │ tag update   │ tag merge + bin   │
 └───────────────┴─────────────┴──────────────┴───────────────────┘
```

Bitmap coalescing combines the structural advantage of buddy (O(1) aligned
search via indexed bins) with the fine-grained size classes of TLSF (low
internal fragmentation) and simple coalescing (no decomposition).

## The key insight: servable sets and the flat bitmap

### Servable sets

A free block at address $A$ with size $n$ can *serve* an allocation of
size class $S$ if:

1. There exists an address $A' \ge A$ such that $A' + S \le A + n$
   (the block is large enough).
2. $A' \bmod \operatorname{align}(S) = 0$ (the address is
   naturally aligned).

The *servable set* of a block is the set of all size classes it can serve.

For $B = 2$ intermediate bits, exhaustive analysis over a 256-chunk arena
(all possible address, size pairs) reveals exactly **34 unique servable
sets**.  These sets have a nearly-linear ordering: within each exponent
group $e$, there are 5 distinct positions:

| Slot      | Can serve                         |
|-----------|-----------------------------------|
| A-only    | $m{=}0$ at exponent $e$, not $m{=}1$ |
| B-only    | $m{=}1$ at exponent $e$, not $m{=}0$ |
| both      | both $m{=}0$ and $m{=}1$          |
| +m2       | also $m{=}2$                      |
| +m3       | all four mantissas                |

A-only and B-only are the sole *incomparable* pair.  Every other pair of
slots is ordered by strict inclusion.

### The flat bitmap

Map each free block to one of the 34 servable-set positions (per exponent
level).  The bitmap layout is:

```
Bit layout:
  [0..2]    PREFIX: sizes 1, 2, 3 (exponents e ≤ 1)
  [3..7]    Exponent e=2: A-only, B-only, both, +m2, +m3
  [8..12]   Exponent e=3: A-only, B-only, both, +m2, +m3
  ...
  Total = 3 + 5 × (MAX_EXPONENT - 1) bits
```

For a 64-bit address space with 16 KiB chunks: MAX_EXPONENT = 49,
NUM_BINS = 243, requiring 4 × 64-bit words.

### Allocation: masked bitmap search

To allocate size class $(e, m)$:

1. Compute the *start bit* — the lowest-indexed bin that could serve this
   request.
2. If $m = 0$: mask out the B-only bit for exponent $e$ (it serves $m=1$
   but not $m=0$).  No other masking is needed.
3. Find the first set bit at or above the start bit.  Pop the head
   of that bin's free list.

This is O(1): one mask operation, one `ctz` per bitmap word.

### Insertion: bin index from (size, alignment)

When inserting a block of size $n$ at address $A$:

1. Compute $\alpha = \operatorname{align}(A / \text{chunk\_size})$
   (the block's chunk-level alignment).
2. For each size class $S$ at each exponent, compute the *threshold*:
   $T(S, \alpha) = S + \max(0, \operatorname{align}(S) - \alpha)$.
   This is the minimum block size needed to guarantee a naturally-aligned
   sub-region of size $S$ exists within the block.
3. The bin index is the highest slot for which $n \ge T(S, \alpha)$ for
   all size classes in that slot's servable set.

Better-aligned blocks reach higher bins (can serve more size classes),
so the bin assignment uses the block's *actual* alignment, not a
worst-case assumption.

## Worked example: bitmap operations

### Arena: 16 chunks at address 0

Initial state: one 16-chunk free block at address 0.

**Insert [0, 16):**
$n = 16$ chunks, $\alpha = \operatorname{align}(0) = \infty$.
At exponent $e = 4$: $S_0 = 16, T = 16$; $S_1 = 20, T = 20 > 16$.
So the block can serve $m{=}0$ at $e{=}4$ but not $m{=}1$.
At exponent $e = 3$: $S_3 = 14, T = 14 \le 16$ → can serve all
four mantissas at $e{=}3$.

Which bin is higher?  A-only at $e{=}4$ (bit index = 3 + 5×2 + 0 = 13)
vs +m3 at $e{=}3$ (bit index = 3 + 5×1 + 4 = 12).  A-only at $e{=}4$
wins.  The block goes to bin 13.

**Allocate 10 chunks ($e{=}3, m{=}1$):**
Start bit = B-only at $e{=}3$ = 3 + 5×1 + 1 = 9.
$m \ne 0$, so no masking.
Scan bitmap from bit 9: bin 13 is set.  Pop the 16-chunk block at
address 0.

The range wrapper carves: $\operatorname{align}(10) = 2$, natural
alignment = 2 chunks.  Address 0 is already 2-aligned.
Return $[0, 10)$, remainder $[10, 16)$ size 6 is re-inserted via
`add_fresh_range`.

**Insert remainder [10, 16):**
$n = 6$, $\alpha = \operatorname{align}(10) = 2$.
Highest qualifying bin: at $e{=}2$, $S_2 = 6, \operatorname{align}(6) = 2$,
$T(6, 2) = 6 \le 6$.  Also $S_1 = 5, T(5, 2) = 5 \le 6$ and
$S_0 = 4, T(4, 2) = 4 \le 6$.  All four check: $S_3 = 7 > 6$, so
stop at +m2.  Bin index = 3 + 5×0 + 3 = 6.

**Allocate 4 chunks ($e{=}2, m{=}0$):**
Start bit = A-only at $e{=}2$ = 3.
$m = 0$: mask out B-only at $e{=}2$ = bit 4.
Scan bitmap from bit 3 with bit 4 masked: bin 6 is set.  Pop the
6-chunk block at address 10.

Carve: $\operatorname{align}(4) = 4$.  Address 10 is 2-aligned,
first 4-aligned address $\ge 10$ is 12.  Return $[12, 16)$, prefix
$[10, 12)$ size 2 is re-inserted.

### Coalescing example

State after the above: $[0, 10)$ allocated, $[10, 12)$ free (bin 1),
$[12, 16)$ allocated.

**Free [12, 16):**
`add_block(12, 4)`.

Left walk: check address 8 ($= 12 - 4$).  Not coalesce_free (it's allocated).
Check address 11 ($= 12 - 1$ chunk).  `is_free_block(11)`: the 2-chunk
free block at $[10, 12)$ has coalesce_free set at address 10 and 11.  Read
$\text{size}(11) = 2$, so $\text{prev\_start} = 12 - 2 = 10$.  Check
`is_free_block(10)`: yes.  Cross-check $\text{size}(10) = 2$: matches.
Remove $[10, 12)$ from its bin.  $\text{merge\_start} = 10$.

Continue left walk: check address 9.  Not coalesce_free (inside the allocated
block $[0, 10)$).  Stop.

Right walk: address 16 is at the boundary.  Stop.

Insert $[10, 16)$ size 6.  The two fragments have coalesced into one
block.

## The algorithm

### Data structures

```
bin_heads[NUM_BINS]    — head pointer for each bin's singly-linked free list
bitmap[BITMAP_WORDS]   — one bit per bin: set iff the bin is non-empty
```

Metadata is stored in the pagemap: each chunk entry has two backend words
(next pointer and size boundary tag) plus two 1-bit flags (coalesce_free and
boundary).

### Insert (`insert_block`)

```
insert_block(addr, size):
    n = size / CHUNK_SIZE
    α = natural_alignment(addr / CHUNK_SIZE)
    bin = bin_index(n, α)
    set_boundary_tags(addr, size)        // first and last entry
    set_coalesce_free(addr)                    // first entry
    if size > CHUNK_SIZE:
        set_coalesce_free(addr + size - CHUNK_SIZE)   // last entry
    prepend to bin_heads[bin]
    set bitmap bit
```

### Allocate (`remove_block`)

```
remove_block(size):
    (e, m) = decompose(size / CHUNK_SIZE)
    start = alloc_start_bit(e, m)
    mask = (m == 0 and e >= 2) ? alloc_mask_bit(e) : none
    bin = find_first_set_bit(bitmap, from=start, masking=mask)
    if none found: return empty
    pop head from bin_heads[bin]
    clear first-entry size tag and coalesce_free marker
    return {addr, block_size}
```

### Free with coalescing (`add_block`)

```
add_block(addr, size):
    merge_start = addr
    merge_end = addr + size

    // Left walk: merge with preceding free blocks
    while merge_start > 0 and not boundary(merge_start):
        prev_last = merge_start - CHUNK_SIZE
        if not is_free_block(prev_last): break
        prev_size = get_size(prev_last)
        if prev_size == 0 or prev_size > merge_start: break
        prev_start = merge_start - prev_size
        if not is_free_block(prev_start): break
        if get_size(prev_start) != prev_size: break     // cross-check
        remove_from_bin(prev_start, prev_size)
        merge_start = prev_start

    // Right walk: merge with following free blocks
    while not boundary(merge_end):
        if not is_free_block(merge_end): break
        next_size = get_size(merge_end)
        if next_size == 0: break
        remove_from_bin(merge_end, next_size)
        clear_size_tags(merge_end, next_size)            // prevent stale reads
        merge_end += next_size

    // Insert the coalesced block
    insert_block(merge_start, merge_end - merge_start)
```

Key property: **no decomposition on free**.  The freed region is maximally
merged with neighbours and inserted as a single block.  The bitmap index
captures exactly which size classes the merged block can serve at its
actual address alignment.

### Post-allocation carving (range wrapper)

The block returned by `remove_block` may not be naturally aligned for the
requested size class.  The range wrapper carves the allocation:

```
alloc_range(size):
    (e, m) = decompose(size / CHUNK_SIZE)
    alignment = natural_alignment(size / CHUNK_SIZE) * CHUNK_SIZE
    result = remove_block(size)
    if empty: refill from parent
    aligned_addr = align_up(result.addr, alignment)
    prefix = aligned_addr - result.addr
    suffix = result.size - prefix - size
    if prefix > 0: add_fresh_range(result.addr, prefix)
    if suffix > 0: add_fresh_range(aligned_addr + size, suffix)
    return aligned_addr
```

## Metadata protocol

### The coalesce-free marker

Each metadata entry carries a single bit, `META_COALESCE_FREE_BIT`, set only on
entries belonging to blocks currently in the coalescing range's free pool.
The coalescing algorithm tests `is_free_block(addr)`, which returns true
only when the entry is both *backend-owned* and has the coalesce-free bit set.
Other range components (e.g. DecayRange) also set backend-owned, but they
do not set coalesce-free, so the coalescing algorithm ignores them.

### Boundary markers

A separate bit, `META_BOUNDARY_BIT`, prevents coalescing from crossing
OS-level allocation boundaries.  On platforms where distinct PAL
allocations cannot be merged (CHERI, Windows VirtualAlloc), the first
chunk of each allocation has its boundary bit set.  Both the left walk
and right walk stop when they encounter a boundary.

When the range wrapper splits a refill (keeping part for the caller,
returning the rest to the free pool), a boundary is set at the split point
to prevent the remainder from coalescing into the returned allocation.

### Marking lifecycle

1. **Insert.** Coalesce-free is set on both the first and last chunk entries.
   Boundary tags (size) are written at both ends.  This allows both
   leftward probing (which reads the last entry of a predecessor) and
   rightward probing (which reads the first entry of a successor).

2. **Allocate (`remove_block`).** The first entry's size tag and coalesce-free
   bit are cleared.  The last entry is left stale.  This is safe because
   the left walk always cross-checks: it reads the predecessor's size from
   its last entry, computes the start address, then verifies
   `is_free_block(start)`.  Since the first entry's coalesce-free was cleared,
   this check fails, preventing incorrect coalescing.

3. **Right-walk absorption.** When the right walk absorbs a neighbour, it
   zeros the absorbed block's first and last size tags.  This prevents
   subsequent left walks from misreading stale sizes at those positions.

| Bit                | Set by               | Cleared by                   | Purpose                    |
|--------------------|-----------------------|------------------------------|----------------------------|
| `META_COALESCE_FREE_BIT` | `insert_block` (both endpoints) | `remove_block` (first only); right-walk (implicit) | Free-pool membership |
| `META_BOUNDARY_BIT`| PAL allocation; range split | Never (preserved)       | Cross-range boundary       |

## B-dependence: structural assumptions

The bitmap layout, mask count, and slot ordering all depend on `B =
INTERMEDIATE_BITS`.  This design hardcodes $B = 2$ and guards every
structural assumption with a `static_assert` labelled **[A1]**–**[A7]**:

| Label | Assumption | What would change for $B \ne 2$ |
|-------|-----------|-------------------------------|
| A1 | 5 slots per exponent | More alignment tiers → more incomparable pairs → more slots |
| A2 | 3 prefix bits (sizes 1–3) | Different sub-exponent range |
| A3 | `alloc_mask_bit` returns one index | Multiple incomparable pairs → multi-bit mask |
| A4 | Mask clears 1 bit | Same as A3 |
| A5 | Only $m{=}0$ needs masking | Intermediate-tier mantissas might also need masks |
| A6 | Slot ordering [A-only, B-only, both, +m2, +m3] | Different DAG shape |
| A7 | Two threshold breakpoints per exponent | More tiers → more breakpoints |

To support a different $B$ value, each of these assumptions would need to
be re-derived (the `single_bitmap.py` and `index_table.py` prototypes in
`prototype/` can be adapted for this).

## File organisation

| File | Purpose |
|------|---------|
| `backend_helpers/bitmap_coalesce_helpers.h` | Pure-math helpers: bin index, threshold, decompose, round-up |
| `backend_helpers/bitmap_coalesce.h` | Core data structure: bitmap, free lists, insert/remove/coalesce |
| `backend_helpers/bitmap_coalesce_range.h` | Pipeline adapter: `alloc_range`/`dealloc_range` with carving and refill |
| `test/func/bc_helpers/bc_helpers.cc` | Unit tests for helper math |
| `test/func/bc_core/bc_core.cc` | Unit tests for core data structure (mock Rep) |
| `test/func/bc_range/bc_range.cc` | Pipeline integration tests |

## Pipeline position

`BitmapCoalesceRange` is wired into snmalloc's range pipeline as the
global coalescing layer:

```
GlobalR = Pipe<
    Base,
    BitmapCoalesceRange<GlobalCacheSizeBits, BITS-1, Pagemap, MinSizeBits>,
    LogRange<2>,
    GlobalRange>
```

It sits between the OS-level base range and the per-thread caching
layers, managing large free blocks and providing naturally-aligned
allocations to the rest of the pipeline.
