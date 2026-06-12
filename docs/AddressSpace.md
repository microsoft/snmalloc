# How snmalloc Manages Address Space

Like any modern, high-performance allocator, `snmalloc` contains multiple layers of allocation.
We give here some notes on the internal orchestration.

## From platform to malloc

Consider a first, "small" allocation (typically less than a platform page); such allocations showcase more of the machinery.
For simplicity, we assume that

- this is not an `OPEN_ENCLAVE` build,
- the `BackendAllocator` has not been told to use a `fixed_range`,
- this is not a `SNMALLOC_CHECK_CLIENT` build, and
- (as a consequence of the above) `SNMALLOC_META_PROTECTED` is not `#define`-d.

Since this is the first allocation, all the internal caches will be empty, and so we will hit all the slow paths.
For simplicity, we gloss over much of the "lazy initialization" that would actually be implied by a first allocation.

1. The `Allocator::small_alloc` finds that it cannot satisfy the request because its lacks a fast free list for this size class.
   The request is delegated, unchanged, to `Allocator::small_refill`.

2. The `Allocator` has no active slab for this sizeclass, so `Allocator::small_refill_slow` delegates to `BackendAllocator::alloc_chunk`.
   At this point, the allocation request is enlarged to one or a few chunks (a small counting number multiple of `MIN_CHUNK_SIZE`, which is typically 16KiB); see `sizeclass_to_slab_size`.

3. `BackendAllocator::alloc_chunk` at this point splits the allocation request in two, allocating both the chunk's metadata structure (of size `PAGEMAP_METADATA_STRUCT_SIZE`) and the chunk itself (a multiple of `MIN_CHUNK_SIZE`).
   Because the two exercise similar bits of machinery, we now track them in parallel in prose despite their sequential nature.

4. The `BackendAllocator` has a chain of "range" types that it uses to manage address space.
   By default (and in the case we are considering), that chain begins with a per-thread *small arena range*.

   1. For the metadata allocation, the size is (well) below `MIN_CHUNK_SIZE` and so this allocator, which by supposition is empty, attempts to `refill` itself from its parent.
      This results in a request for a `MIN_CHUNK_SIZE` chunk from the parent allocator.

   2. For the chunk allocation, the size is `MIN_CHUNK_SIZE` or larger, so this allocator immediately forwards the request to its parent.

5. The next range allocator in the chain is a per-thread `LargeArenaRange` that refills in 2 MiB granules.
   (2 MiB chosen because it is a typical superpage size.)
   At this point, both requests are for at least one and no more than a few times `MIN_CHUNK_SIZE` bytes.

   1. The first request will `refill` this empty allocator by making a request for 2 MiB to its parent.

   2. The second request will stop here, as the allocator will no longer be empty.

6. The chain continues with a `CommitRange`, which simply forwards all allocation requests and (upon unwinding) ensures that the address space is mapped.

7. The chain now transitions from thread-local to global; the `GlobalRange` simply serves to acquire a lock around the rest of the chain.

8. The next entry in the chain is a `StatsRange` which serves to accumulate statistics.
   We ignore this stage and continue onwards.

9. The next entry in the chain is another `LargeArenaRange` which refills at 16 MiB but can hold regions
   of any size up to the entire address space.
   The first request triggers a `refill`, continuing along the chain as a 16 MiB request.
   (Recall that the second allocation will be handled at an earlier point on the chain.)

10. The penultimate entry in the chain is a `PagemapRegisterRange`, which always forwards allocations along the chain.

11. At long last, we have arrived at the last entry in the chain, a `PalRange`.
    This delegates the actual allocation, of 16 MiB, to either the `reserve_aligned` or `reserve` method of the Platform Abstraction Layer (PAL).

12. Having wound the chain onto our stack, we now unwind!
    The `PagemapRegisterRange` ensures that the Pagemap entries for allocations passing through it are mapped and returns the allocation unaltered.

13. The global `LargeArenaRange` carves the request out of its 16 MiB refill and keeps the unused remainder as a single free block in its internal red-black trees of free ranges, returning the carved portion back along the chain.

14. The `StatsRange` makes its observations, the `GlobalRange` now unlocks the global component of the chain, and the `CommitRange` ensures that the allocation is mapped.
    Aside from these side effects, these propagate the allocation along the chain unaltered.

15. We now arrive back at the thread-local `LargeArenaRange`, which takes its 2 MiB refill and carves out the requested chunk(s); the unused remainder stays in its free-range trees.
    The second allocation (of the chunk) will either be satisfied from this leftover or trigger another carve.

16. For the first (metadata) allocation, the thread-local *small arena range* takes its `MIN_CHUNK_SIZE` refill, hands back a sub-chunk fragment large enough for `PAGEMAP_METADATA_STRUCT_SIZE`, and tracks the remainder as free sub-chunk space using tree nodes stored inside the free fragments themselves.
    The second allocation will have been forwarded and so is not additionally handled here.

Exciting, no?

## What Can I Learn from the Pagemap?

### Decoding a MetaEntry

The centerpiece of `snmalloc`'s metadata is its `Pagemap`, which associates each "chunk" of the address space (~16KiB; see `MIN_CHUNK_BITS`) with a `MetaEntry`.
A `MetaEntry` is a pair of pointers, suggestively named `meta` and `remote_and_sizeclass`.
In more detail, `MetaEntry`s are better represented by Sigma and Pi types, all packed into two pointer-sized words in ways that preserve pointer provenance on CHERI.

To begin decoding, a bit (`REMOTE_BACKEND_MARKER`) in `remote_and_sizeclass` distinguishes chunks owned by frontend and backend allocators.

For chunks owned by the *frontend* (`REMOTE_BACKEND_MARKER` not asserted),

1. The `remote_and_sizeclass` field is a product of

   1. A `RemoteAllocator*` indicating the `LocalAlloc` that owns the region of memory.

   2. A "full sizeclass" value (itself a tagged sum type between large and small sizeclasses).

2. The `meta` pointer is a bit-stuffed pair of

   1. A pointer to a larger metadata structure with type dependent on the role of this chunk

   2. A bit (`META_BOUNDARY_BIT`) that serves to limit chunk coalescing on platforms where that may not be possible, such as CHERI.

See `src/snmalloc/mem/metadata.h`.

For chunks owned by a *backend* (`REMOTE_BACKEND_MARKER` asserted), there are again multiple possibilities.

For chunks owned by a *small arena range* (`SmallArenaRange`), the remainder of the `MetaEntry` is zero.
That is, it appears to have small sizeclass 0 and an implausible `RemoteAllocator*`.
The free-fragment tree itself is stored in-band, inside the free space of the chunk, rather than in the pagemap (see `InplaceRep` in `src/snmalloc/backend_helpers/inplacerep.h`).

For chunks owned by a `LargeArenaRange`, the `MetaEntry` is instead a node in the red-black trees of free ranges.
A free block of *N* units consumes the `MetaEntry`s of its first *min(N, 3)* unit-aligned addresses; their words encode the bin-tree node (unit 0), the range-tree node (unit 1, for blocks of two or more units), and the large-chunk count (unit 2, for blocks of three or more units).
The pagemap reserves the low `MetaEntryBase::BACKEND_LAYOUT_FIRST_FREE_BIT` bits of each word for the meta-entry layout itself; the tree-node encoding (left/right pointers, red bit, variant tag, large-size count) lives at or above that bit.

See `PagemapRep` in `src/snmalloc/backend_helpers/largearenarange.h`.

### Encoding a MetaEntry

We can also consider the process for generating a MetaEntry for a chunk of the address space given its state.
The following cases apply:

1. The address is not associated with `snmalloc`:
   Here, the `MetaEntry`, if it is mapped, is all zeros and so it...
   * has `REMOTE_BACKEND_MARKER` clear in `remote_and_sizeclass`.
   * appears to be owned by a frontend RemoteAllocator at address 0 (probably, but not certainly, `nullptr`).
   * has "small" sizeclass 0, which has size 0.
   * has no associated metadata structure.

2. The address is part of a free chunk in a backend `LargeArenaRange`:
   The `MetaEntry`...
   * has `REMOTE_BACKEND_MARKER` asserted in `remote_and_sizeclass`.
   * has "small" sizeclass 0, which has size 0.
   * the remainder of its `MetaEntry` structure (and those of the next one or two unit-aligned `MetaEntry`s if the free block spans them) carries the `Arena`'s red-black-tree node encoding.
   * has no associated metadata structure.

3. The address is part of a free fragment inside a backend `SmallArenaRange`:
   Here, the `MetaEntry` is zero aside from the asserted `REMOTE_BACKEND_MARKER` bit, and so it...
   * has "small" sizeclass 0, which has size 0.
   * has no associated metadata structure.

   The tree of free sub-chunk fragments for this chunk is stored inside the free fragments themselves (`InplaceRep`), not in the pagemap.

4. The address is part of a live large allocation (spanning one or more 16KiB chunks):
   Here, the `MetaEntry`...
   * has `REMOTE_BACKEND_MARKER` clear in `remote_and_sizeclass`.
   * has a *large* sizeclass value.
   * has an associated `RemoteAllocator*` and `Metaslab*` metadata structure
     (holding just the original chunk pointer in its `MetaCommon` substructure;
      it is configured to always trigger the deallocation slow-path to skip the logic when a chunk is in use as a slab).

5. The address, whether or not it is presently within an allocated object, is part of an active slab.  Here, the `MetaEntry`....
   * encodes the *small* sizeclass of all objects in the slab.
   * has a `RemoteAllocator*` referencing the owning `LocalAlloc`'s message queue.
   * points to the slab's `Metaslab` structure containing additional metadata (e.g., free list).
