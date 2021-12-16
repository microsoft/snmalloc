# StrictProvenance Architectures

To aid support of novel architectures, such as CHERI, which explicitly track pointer *provenance* and *bounds*, `snmalloc` makes heavy use of a `CapPtr<T, B>` wrapper type around `T*` values.
You can view the annotation `B` on a `CapPtr<T, B>` as characterising the set of operations that are supported on this pointer, such as

* address arithmetic within a certain range (e.g, a `Superslab` chunk)
* requesting manipulation of the virtual memory mappings

Most architectures and platforms cannot enforce these restrictions, but CHERI enables software to constrain its future use of particular pointers and `snmalloc` imposes strong constraints on its *client(s)* use of memory it manages.
The remainder of this document...

* gives a "quick start" guide,
* provides a summary of the constraints imposed on clients,
* motivates and introduces the internal `ArenaMap` structure and the `capptr_amplify` function, and
* describes the `StrictProvenance` `capptr_*` functions provided by the Architecture Abstraction Layer (AAL) and Platform Abstraction Layer (PAL).

## Preface

The `CapPtr<T, B>` and `capptr_*` primitives and derived functions are intended to guide developers in useful directions; they are not security mechanisms in and of themselves.
For non-CHERI architectures, the whole edifice crumbles in the face of an overzealous `reinterpret_cast<>` or `unsafe_capptr` member access.
On CHERI, these are likely to elicit capability violations, but may not if all subsequent access happen to be within bounds.

## Quick Start Guide

### How do I safely get an ordinary pointer to reveal to the client?

If you are...

* Adding an interface like `external_pointer`, and so you have a `CapPtr<T, CBAllocE>`, `e`, whose bounds you want to *inherit* when revealing some other `CapPtr` `p`, use `capptr_rebound(e, p)` to obtain another `CapPtr<T, CBAllocE>` with address from `p`, then go to the last step here.

* Otherwise, if your object is...

  * an entire `SUPERSLAB_SIZE` chunk or bigger, you should have in hand a `CapPtr<T, CBChunk>` from the large allocator.  Use `capptr_export` to make a `CapPtr<T, CBChunkE>`, then use `capptr_chunk_is_alloc` to convert that to a `CapPtr<T, CBAllocE>`, and then proceed.  (If, instead, you find yourself holding a `CapPtr<T, CBChunkD>`, use `capptr_chunk_from_chunkd` first.)

  * of size `sz` and smaller than such a chunk,

    * and have a `CapPtr<T, CBChunkE> p` in hand, use `Aal::capptr_bound<T, CBAllocE>(p, sz)` to get a `CapPtr<T, CBAllocE>`, and then proceed.

    * an have a `CapPtr<T, CBArena> p`, `CapPtr<T, CBChunkD> p`, or `CapPtr<T, CBChunk> p` in hand, use `Aal::capptr_bound<T, CBAlloc>(p, sz)` to get a `CapPtr<T, CBAlloc>`, and then proceed.

* If the above steps left you with a `CapPtr<T, CBAlloc>`, apply any platform constraints for its export with `Pal::capptr_export(p)` to obtain a `CapPtr<T, CBAllocE>`.

* Use `capptr_reveal` to safely convert a `CapPtr<T, CBAllocE>` to a `T*` for the client.

### How do I safely ingest an ordinary pointer from the client?

For all its majesty, `CapPtr`'s coverage is merely an impediment to, rather than a complete defense against, malicious client behavior even on CHERI-enabled architectures.
Further protection is an open research project at MSR.

Nevertheless, if adding a new kind of deallocation, we suggest following the existing flows:

* Begin by wrapping it with `CapPtr<T, CBAllocE>` and avoid using the raw `T*` thereafter.

* An `CapPtr<T, CBArena>` can be obtained using `large_allocator.capptr_amplify()`.
Note that this pointer and its progeny are *unsafe* beyond merely having elevated authority: it is possible to construct and dereference pointers with types that do not match memory, resulting in **undefined behavior**.

* Derive the `ChunkMapSuperslabKind` associated with the putative pointer from the client, by reading the `ChunkMap`.
In some flows, the client will have made a *claim* as to the size (class) of the object which may be tentatively used, but should be validated (unless the client is trusted).

* Based on the above, for non-Large objects, `::get()` the appropriate header structure (`Superslab` or `Mediumslab`).

Eventually we would like to reliably detect references to free objects as part of these flows, especially as frees can change the type of metadata found at the head of a chunk.
When that is possible, we will add guidance that only reads of non-pointer scalar types are to be performed until after such tests have confirmed the object's liveness.
Until then, we have stochastic defenses (e.g., `encode` in `src/mem/freelist.h`) later on.

As alluded to above, `capptr_rebound` can be used to ensure that pointers manipulated within `snmalloc` inherit bounds from client-provided pointers.
In the future, these derived pointers will inherit *temporal bounds* as well as the spatial ones described herein.

### What happened to my cast operators?

Because `CapPtr<T, B>` are not the kinds of pointers C++ expects to manipulate, `static_cast<>` and `reinterpret_cast<>` are not applicable.
Instead, `CapPtr<T, B>` exposes `as_void()`, `template as_static<U>()`, and `template as_reinterpret<U>()` to perform `static_cast<void*>`, `static_cast<U*>`, and `reinterpret_cast<U*>` (respectively).
Please use the first viable option from this list, reserving `reinterpret_cast` for more exciting circumstances.

## StrictProvenance in More Detail

Tracking pointer *provenance* and *bounds* enables software to constrain uses of *particular pointers* in ways that are not available with traditional protection mechanisms.
For example, while code my *have* a pointer that spans its entire C stack, it may construct a pointer that authorizes access only to a particular stack allocation (e.g., a buffer) and use this latter pointer while copying data.
Even if an attacker is able to control the length of the copy, the bounds imposed upon pointers involved can ensure that an overflow is impossible.
(Of course, if the attacker can influence both the *bounds* and the copy length, an overflow may still be possible; in practice, however, the two concerns are often sufficiently separated.)
For `malloc()` in particular, it is enormously beneficial to be able to impose bounds on returned pointers: it becomes impossible for allocator clients to use a pointer from `malloc()` to access adjacent allocations!

Borrowing terminology from CHERI, we speak of the **authority** (to a subset of the address space) held by a pointer and will justify actions in terms of this authority.
While many kinds of authority can be envisioned, herein we will mean either

* *spatial* authority to read/write/execute within a single *interval* within the address space, or
* *vmmap* authority to request modification of the virtual page mappings for a given range of addresses.

We may **bound** the authority of a pointer, deriving a new pointer with a subset of its progenitor's authority; this is assumed to be an ambient action requiring no additional authority.
Dually, given two pointers, one with a subset of the other's authority, we may **amplify** the less-authorized, constructing a pointer with the same address but with increased authority (up to the held superset authority).[^amplifier-state]

## Constraints Imposed Upon Allocations

`snmalloc` ensures that returned pointers are bounded to no more than the slab entry used to back each allocation.
It may be useful, mostly for debugging, to more precisely bound returned pointers to the actual allocation size,[^bounds-precision] but this is not required for security.
The pointers returned from `alloc()` will be stripped of their *vmmap* authority, if supported by the platform, ensuring that clients cannot manipulate the page mapping underlying `snmalloc`'s address space.

`realloc()`-ation has several policies that may be sensible.
We choose a fairly simple one for the moment: resizing in ways that do not change the backing allocation's `snmalloc` size class are left in place, while any change to the size class triggers an allocate-copy-deallocate sequence.
Even if `realloc()` leaves the object in place, the returned pointer should have its authority bounded as if this were a new allocation (and so may have less authority than `realloc()`'s pointer argument if sub-slab-entry bounds are being applied).
(Notably, this policy is compatible with the existence of size-parameterized deallocation functions: the result of `realloc()` is always associated with the size class corresponding to the requested size.
By contrast, shrinking in place in ways that changed the size class would require tracking the largest size ever associated with the allocation.)

## Impact of Constraints On Deallocation, or Introducing the ArenaMap

Strict provenance and bounded returns from `alloc()` imply that we cannot expect things like

```c++
void dealloc(void *p)
{
  Superslab *super = Superslab::get(p);
  ... super->foo ...
}
```

to work (using the existing `Superslab::get()` implementation).
Architecturally, `dealloc` is no different from any *allocator client* code and `Superslab::get()` is merely some pointer math.
As such, `Superslab::get()` must either fail to construct its return value (e.g., by trapping) or construct a useless return value (e.g., one that traps on dereference).
To proceed, we must take advantage of the fact that `snmalloc` has separate authority to the memory underlying its allocations.

Ultimately, all address space manipulated by `snmalloc` comes from its Platform's primitive allocator.
An **arena** is a region returned by that provider.
The `AddressSpaceManager` divides arenas into large allocations and manages their life cycles.
On `StrictProvenance` architectures, the ASM further maintains a map of all PAL-provided memory, called the `ArenaMap`, and uses this to implement `capptr_amplify`, copying the address of a low-authority pointer into a copy of the high-authority pointer provided by the PAL.
The resulting pointer can then be used much as on non-`StrictProvenance` architectures, with integer arithmetic being used to make it point anywhere within an arena.
`snmalloc`'s heap layouts ensure that metadata associated with any object are spread across globals and within the same arena as the object itself, and so, assuming access to globals as given, a single amplification suffices.

## Object Lookup

`snmalloc` extends the traditional allocator interface with the `template<Boundary> void* external_pointer(void*)` family of functions, which generate additional pointers to live allocations.
To ensure that this function is not used as an amplification oracle, it must construct a return pointer with the same validity as its input even as it internally amplifies to access metadata; see `capptr_rebound`.

XXX It may be worth requiring that the input pointer authorize the entire object?
What are the desired security properties here?

# Adapting the Implementation

## Design Overview

As mentioned, the `AddressSpaceManager` maintains an `ArenaMap`, a cache of pointers that span the entire heap managed by `snmalloc`.
To keep this cache small, we request very large swaths (GiB-scale on >48-bit ASes) of address space at a time, even if we only populate those regions very slowly.

Within `snmalloc`, there are several data structures that hold free memory:

* the `LargeAlloc` holds all regions too big to be managed by `MediumSlab`s

* `MediumSlab`s hold free lists

* `Slab`s hold free lists.

* `Slab`s have associated "bump pointer" regions of address space not yet used (facilitating lazy construction of free lists)

* `Alloc`s themselves also hold, per small size class, up to one free list and up to one bump pointer (so that the complexity of `Slab` manipulation is amortized across many allocations)

* `Alloc`s have or point to `RemoteAllocator`s, which contain queues of `Remote` objects formed from deallocated memory.

* `Alloc`s have `RemoteCaches` that also hold `Remote`s.

We take the position that free list entries should be suitable for return, i.e., with authority bounded to their backing slab entry.
(However, the *contents* of free memory may be dangerous to expose to the user and require clearing prior to handing out.)
This means that allocation fast paths are unaffected by the requirement to bound return pointers, but that deallocation paths may need to amplify twice, once on receipt of the pointer from the application and again on receipt of the pointer from another `Allocator` through the `Remote` mechanism.

## Static Pointer Bound Taxonomy

At the moment, we introduce six possible annotations, though the taxonomy is imperfect:

* bounded only to an underlying arena without platform constraints, `CBArena`;
* bounded to a `SUPERSLAB_SIZE` or larger chunk without platform constraints, `CBChunk`;
* bounded to a `SUPERSLAB_SIZE` or larger chunk with platform constraints, `CBChunkE`;
* bounded *on debug builds* to a `SUPERSLAB_SIZE` or larger chunk without platform constraints, `CBChunkD`;
* bounded to an allocation but without platform constraints yet applied, `CBAlloc`;
* bounded to an allocation and with platform constraints, `CBAllocE`;

By "platform constraints" we mean, for example, CheriBSD's ability to remove the authority to manage the VM mappings underlying a pointer.
Clients of malloc have no business attempting to manage the backing pages.

In practice, we use the pair of the type `T` and the bounds annotation for additional light-weight verification.
For example, we differentiate `CapPtr<Remote, CBAlloc>` from `CapPtr<void, CBAlloc>`, with the former being offset (if cache-friendly offsets are in effect) and the latter almost always pointing to the start of the object. 
While it is possible to write code which subverts the annotation scheme, in general method signatures should provide the correct affordance.

## Primitive Architectural Operations

Several new functions are introduced to AALs to capture primitives of the Architecture.

* `CapPtr<T, nbounds> capptr_bound(CapPtr<U, obounds> a, size_t sz)`
  spatially bounds the pointer `a` to have authority ranging only from its current target to its current target plus `sz` bytes (which must be within `a`'s authority).
  No imprecision in authority is permitted.
  The `obounds` annotation is required to be either strictly higher authority than `CBAlloc` or `CBChunkE`, and the bounds annotations must obey `capptr_is_bounds_refinement`.

* `CapPtr<T, BOut> capptr_rebound(CapPtr<void, BOut> a, CapPtr<T, BIn> p)` is the *architectural primitive* enabling the software amplification mechanism.
  It combines the authority of `a` and the current target of `p`.
  The result may be safely dereferenced iff `a` authorizes access to `p`'s target.
  The simplest sufficient (but not necessary) condition to ensure safety is that authority of `a` is a superset of the authority of `p` and `p` points within its authority.

## Primitive Platform Operations

* `CapPtr<void, BO> capptr_export(CapPtr<T, BI> f)` applies any additional platform constraints required before handing permissions out to the client.
On CheriBSD, specifically, this strips the `VMMAP` software permission, ensuring that clients cannot have the kernel manipulate heap pages.
In future architectures, this is increasingly likely to be a no-op.
The annotation `BO` is *computed* as a function of `BI`, which must be `CBChunk` or `CBAlloc`.

## Constructed Operators

* `capptr_bound_chunkd` and `capptr_chunk_from_chunkd` manage the construction and elimination of `CapPtr<T, CBChunkD>` pointers.

* `capptr_chunk_is_alloc` converts a `CapPtr<T, CBChunkE>` to a `CapPtr<T, CBAllocE>` unsafely; it is intended to ease auditing.

* `capptr_reveal` converts a `CapPtr<T, CBAllocE>` to a `void*`.

## Amplification

The `AddressSpaceManager` now exposes a method with signature `CapPtr<T, CBArena> capptr_amplify(CapPtr<void, B> p)` which uses `capptr_rebound` to construct a pointer targeting `p`'s target but bearing the authority of the primordial allocation granule (as provided by the kernel) containing this address.
This pointer can be used to reach the `Allocslab` metadata associated with `p` (and a good bit more, besides!).

# Endnotes

[^mmu-perms] Pointer authority generally *intersects* with MMU-based authorization.
For example, software using a pointer with both write and execute authority will still find that it cannot write to pages considered read-only by the MMU nor will it be able to execute non-executable pages.
Generally speaking, `snmalloc` requires only read-write access to memory it manages and merely passes through other permissions, with the exception of *vmmap*, which it removes from any pointer it returns.

[^amplifier-state] As we are largely following the fat pointer model and its evolution into CHERI capabilities, we achieve amplification through a *stateful*, *software* mechanism, rather than an architectural mechanism.
Specifically, the amplification mechanism will retain a superset of any authority it may be asked to reconstruct.
There have, in times past, been capability systems with architectural amplification (e.g., HYDRA's type-directed amplification), but we believe that future systems are unlikely to adopt this latter approach, necessitating the changes we propose below.

[^bounds-precision] `StrictProvenance` architectures have historically differed in the precision with which authority can be represented.
Notably, it may not be possible to achieve byte-granular authority boundaries at every size scale.
In the case of CHERI specifically, `snmalloc`'s size classes and its alignment policies are already much coarser than existing architectural requirements for representable authority on all existing implementations.


