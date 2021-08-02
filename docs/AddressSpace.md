# How snmalloc Manages Address Space

Like any modern, high-performance allocator, `snmalloc` contains multiple layers of allocation.
We give here some notes on the internal orchestration.

## From platform to malloc

Consider a first, "small" allocation (typically less than a platform page); such allocations showcase more of the machinery.
Since this is the first allocation, all the internal caches will be empty, and so we will hit all the slow paths.
For simplicity, we gloss over much of the "lazy initialization" that would actually be implied by a first allocation.

1. The `LocalAlloc::small_alloc` finds that it cannot satisfy the request because its `LocalCache` lacks a free list for this size class.
   The request is delegated, unchanged, to `CoreAllocator::small_alloc`.

2. The `CoreAllocator` has no active slab for this sizeclass, so `CoreAllocator::small_alloc_slow` delegates to the `ChunkAllocator`.
   At this point, the allocation request is enlarged to be one (or more) chunks (typically multiples of 16KiB).

3. `ChunkAllocator::alloc_chunk` has no free chunks for slabs holding objects of the original sizeclass, so delegates the request to the `Backend`.

4. `Backend::alloc_chunk` calls `Backend::reserve` which appeals to its local `AddressSpaceManagerCore` cache and fails.

5. `Backend::alloc_chunk` then appeals to the global `AddressSpaceManager`.
   The request is again enlarged to the "refill size" of 2MiB (a typical superpage size).

6. `AddressSpaceManager::reserve_with_left_over` and `AddressSpaceManager::reserve` delegate to the platform through the Platform Abstraction Layer (PAL).
   These methods may enlarge the allocation further at the behest of the platform.

7. If the global `AddressSpaceManager` enlarged the request, it will split the platform-provided memory region, caching the remainder (in its `AddressSpaceManagerCore`) for subsequent allocations.
   (The free lists holding any remainder regions here are encoded into the Pagemap.)

8. `Backend::reserve` then, again, may split off a piece of the provided region, storing the rest in its local `AddressSpaceManagerCore`.
   (The free lists holding any remainder regions here are encoded into the Pagemap, again.)

9. `Backend::alloc_chunk` additionally allocates a `Metaslab` and registers this with the `Pagemap`, together with the slab sizeclass value and a pointer to this `LocalAlloc`'s `RemoteAllocator` public state.

10. `CoreAllocator::small_alloc_slow` constructs a free list in the slab and returns the whole list to the local `LocalCache`.

11. `LocalAlloc::small_alloc` now takes the head of this free list and returns that allocation to the user.

## What Can I Learn from the Pagemap?

The centerpiece of `snmalloc`'s metadata is its `PageMap`, which associates each "chunk" of the address space (~16KiB; see `MIN_CHUNK_BITS`) with a `MetaEntry`.
A `MetaEntry` is a pair of pointers; the first uses offset manipulation to densely encode...

1. A `sizeclass` field encoding one of...

   1. zero, if no allocations exist in, or spanning, this chunk; or
   2. the *small* `sizeclass` of the slab in this chunk, if a `LocalAlloc` owns it; or
   3. the *large* `sizeclass` for this chunk-spanning allocation, if no `LocalAlloc` is associated.

2. A `RemoteAllocator*` indicating the `LocalAlloc` that owns the region of memory, if any.

The other pointer targets either...

1. the `Metaslab` record for the slab in this chunk, if it is part of a slab (either active or free in the `ChunkAllocator`), or
2. the next free chunk in an `AddressSpaceManagerCore` per-sizeclass queue, or
3. `nullptr` or `0` otherwise

For a given address, there are five cases to consider:

1. The address is not associated with `snmalloc`.  Here, the `MetaEntry`, if it is mapped, is all zeros and so...
   * has a `sizeclass` of 0,
   * has an all-zeros `RemoteAllocator*` (i.e., probably `nullptr`), and
   * has an all-zeros ancillary pointer.

2. The address is part of a free chunk in any of the `AddressSpaceManagerCore`s.  Here, the `MetaEntry`...
   * has a `sizeclass` of `0`,
   * has a `nullptr` `RemoteAllocator*`, and
   * is carrying the pointer to the next free chunk in the same size class.

3. The address is part of a live large allocation (spanning one or more 16KiB chunks).  Here, the `MetaEntry`...
   * encodes the *large* allocation's non-zero sizeclass (and so alignment),
   * has a `nullptr` `RemoteAllocator*`, and
   * does not use its pointer field.

4. The address is part of a free slab inside a `ChunkAllocator`.  Here, the `MetaEntry`...
   * has a `sizeclass` of `0`,
   * has a `nullptr` `RemoteAllocator*`, and
   * is carrying the pointer to the next free slab of the same size class.

5. The address, whether or not it is presently within an allocated object, is part of an active slab.  Here, the `MetaEntry`....
   * encodes the *small* sizeclass of all objects in the slab,
   * has a `RemoteAllocator*` referencing the owning `LocalAlloc`'s message queue, and
   * points to the slab's `Metaslab` structure containing additional metadata (e.g., free list).

The `Pagemap` itself is not sufficient to distinguish all these cases, and, indeed, is not guaranteed to be mapped for regions not managed by `snmalloc`!
However, assuming the `MetaEntry` is mapped, if the pointer encoding `RemoteAllocator*` and `sizeclass` is `nullptr` (or bitwise `0`) then case 2 or 4 (or 1) applied at the moment that field was read;
the `Pagemap` cannot further separate these.
Otherwise, the `RemoteAllocator*` distinguishes between large and small allocations.
(Some care is required: large allocations encode a their `sizeclass` by "offsetting from `nullptr`", which is undefined behavior; the actual calculation is performed at `uintptr_t`, not `RemoteAllocator*`, and it is
assumed that `nullptr` is at least as aligned as any `RemoteAllocator` is required to be.)
In fact, this combined field is sufficient to find the size and alignment of putative objects at a given address without reading any additional metadata; see, `LocalAlloc::alloc_size` and `LocalAlloc::external_pointer`.
The results of those functions are, nevertheless, only sensible when given `nullptr` or a *live* allocation.

