# StrictProvenance Architectures

## What is Strict Provenance?

Some architectures, such as CHERI (including Arm's Morello), explicitly consider pointer *provenance* and *bounds* in addition to their target *addresses*.
Adding these considerations to the architecture enables software to constrain uses of *particular pointers* in ways that are not available with traditional protection mechanisms.
For example, while code may *have* a pointer that spans its entire C stack, it may construct a pointer that authorizes access only to a particular stack allocation (e.g., a buffer) and use this latter pointer while copying data.
Even if an attacker is able to control the length of the copy, the bounds imposed upon pointers involved can ensure that an overflow is impossible.
(On the other hand, if the attacker can influence both the *bounds* and the copy length, an overflow may still be possible; in practice, however, the two concerns are often sufficiently separated.)
For `malloc()` in particular, it is enormously beneficial to be able to impose bounds on returned pointers: it becomes impossible for allocator clients to use a pointer from `malloc()` to access adjacent allocations!
(*Temporal* concerns still apply, in that live allocations can overlap prior, now-dead allocations.
Stochastic defenses are employed within `snmalloc` and deterministic defenses are ongoing research at MSR.)

Borrowing terminology from CHERI, we speak of the **authority** (to a subset of the address space) held by a pointer and will justify actions in terms of this authority.[^mmu-perms]
While many kinds of authority can be envisioned, herein we will mean either

* *spatial* authority to read/write/execute within a single *interval* within the address space, or
* *vmem* authority to request modification of the virtual page mappings for a given range of addresses.

We may **bound** the authority of a pointer, deriving a new pointer with a subset of its progenitor's authority; this is assumed to be an ambient action requiring no additional authority.
Dually, given two pointers, one with a subset of the other's authority, we may **amplify** the less-authorized, constructing a pointer with the same address but with increased authority (up to the held superset authority).[^amplifier-state]

## snmalloc Support For Strict Provenance

### Static Annotations With CapPtr

To aid code auditing, `snmalloc` makes heavy use of a `CapPtr<T, B>` wrapper type around `T*` values.
You can think of the annotation `B` on a `CapPtr<T, B>` as capturing something about the role of the pointer, e.g.:

* A pointer to a whole chunk or slab, derived from an internal `void*`.
* A pointer to a particular allocation, destined for the user program
* A putative pointer returned from the user program

You can also view the annotation `B` as characterising the set of operations that are supported on this pointer, such as

* nothing (because we haven't checked that it's actually a valid pointer)
* memory access within a certain range (e.g, a chunk or an allocation)
* requesting manipulation of the virtual memory mappings

Most architectures and platforms cannot enforce these restrictions outside of static constraints, but CHERI enables software to constrain its future use of particular pointers and `snmalloc` imposes strong constraints on its *client(s)* use of memory it manages.
The remainder of this document...

* gives a "quick start" guide,
* provides a summary of the constraints imposed on clients,
* describes the `StrictProvenance` `capptr_*` functions provided by `ds/ptrwrap.h`, the Architecture Abstraction Layer (AAL), and the Platform Abstraction Layer (PAL).

### Limitations

The `CapPtr<T, B>` and `capptr_*` primitives and derived functions are intended to guide developers in useful directions; they are not security mechanisms in and of themselves.
For non-CHERI architectures, the whole edifice crumbles in the face of an overzealous `reinterpret_cast<>` or `unsafe_*ptr` call.
On CHERI, these are likely to elicit capability violations, but may not if all subsequent access happen to be within the architecturally-enforced bounds.

## Quick Start Guide

### What will I see?

In practice, `CapPtr<T, B>` and the details of `B` overtly show themselves only in primitive operations or when polymorphism across `B` is required.
(Or, sadly, when looking at compilation errors or demangled names in a debugger.)
All the concrete forms we have found useful have layers of aliasing to keep the verbosity down: `capptr::B<T>` is a `CapPtr<T, capptr::bounds::B>` with `capptr::bounds::B` itself an alias for a `capptr::bound<...>` type-level object.
This trend of aliasing continues into higher-level abstractions, such as the freelist, wherein one finds, for example, `freelist::HeadPtr`, which expands to a type involving several `CapPtr`s and associated annotations.

### How do I safely get an ordinary pointer to reveal to the client?

Almost all memory manipulated by `snmalloc` frontends comes via the backend's `alloc_chunk` method.
The returned a `capptr::Chunk<void>`; this pointer is spatially bounded to the returned region (which is at least as big as requested).
This pointer is, however, not restricted in its ability to manipulate the address space within its bounds; this permits the frontend to call `mmap` and `madvise` on pages therein.

To derive a pointer that is suitable for client use, we must

* further spatially refine the pointer: adjust its offset with `pointer_offset` and use `capptr_bound<T, capptr::bounds::AllocFull>` and
* shed address space control: use `PAL::capptr_to_user_address_control()` to convert `AllocFull` to `Alloc`.

If no additional spatial refinement is required, because the entire chunk is intended for client use,

* shed address space control: use `PAL::capptr_to_user_address_control()` to obtain a `ChunkUser`-bounded pointer, then
* use `capptr_chunk_is_alloc` to capture intent, converting `ChunkUser` to `Alloc` without architectural consequence.

At this point, we hold a `capptr::Alloc<T>`; use `capptr_reveal()` to obtain the underlying `T*`.

### How do I safely ingest an ordinary pointer from the client?

First, we must admit that, for all its majesty, `CapPtr`'s coverage is merely an impediment to, rather than a complete defense against, malicious client behavior even on CHERI-enabled architectures.
Further protection is an open research project at MSR.

Nevertheless, if adding a new kind of deallocation, we suggest following the existing flows when given a `void* p_raw` from the client:

* Begin by calling `p_wild = capptr_from_client(p_raw)` to annotate it as `AllocWild` and avoid using the raw form thereafter.

* Check the `Wild` pointer for domestication with `p_tame = capptr_domesticate<Config>(state_ptr, p_wild)`; `p_tame` will be a `capptr::Alloc` and will alias `p_wild` or will be `nullptr`.
  At this point, we have no more use for `p_wild`.

* We may now probe the Pagemap; either `p_tame` is a pointer we have given out or `nullptr`, or this access may trap (especially on platforms where domestication is just a rubber stamp).
  This will give us access to the associated `MetaEntry` and, in general, a (path to a) `Chunk`-bounded pointer to the entire backing region.

* If desired, we can now validate other attributes of the provided capability, including its length, base, and permissions.
  In fact, we can even go further and *reconstruct* the capability we would have given out for the indicated allocation, allowing for exact comparison.

Eventually we would like to reliably detect references to free objects as part of these flows, especially as frees can change the type of metadata found at the head of a chunk.
When that is possible, we will add guidance that only reads of non-pointer scalar types are to be performed until after such tests have confirmed the object's liveness.
Until then, we have stochastic defenses (e.g., `encode` in `src/mem/freelist.h`) later on.

### What happened to my cast operators?

Because `CapPtr<T, B>` are not the kinds of pointers C++ expects to manipulate, `static_cast<>` and `reinterpret_cast<>` are not applicable.
Instead, `CapPtr<T, B>` exposes `as_void()`, `template as_static<U>()`, and `template as_reinterpret<U>()` to perform `static_cast<void*>`, `static_cast<U*>`, and `reinterpret_cast<U*>` (respectively).
Please use the first viable option from this list, reserving `reinterpret_cast` for more exciting circumstances.

## Constraints Imposed Upon Allocations

`snmalloc` ensures that returned pointers are bounded to no more than the slab entry used to back each allocation.
That is, **no two live allocations will have overlapping bounds**.
It may be useful, mostly for debugging, to more precisely bound returned pointers to the actual allocation size,[^bounds-precision] but this is not required for security.
The pointers returned from `alloc()` will also be stripped of their *vmem* authority, if supported by the platform, ensuring that clients cannot manipulate the page mapping underlying `snmalloc`'s address space.

`realloc()`-ation has several policies that may be sensible.
As a holdover from snmalloc v1, we have a fairly simple policy: resizing in ways that do not change the backing allocation's `snmalloc` size class are left in place, while any change to the size class triggers an allocate-copy-deallocate sequence.
Even if `realloc()` leaves the object in place, the returned pointer should have its authority bounded as if this were a new allocation (and so the result may be a subset of the input to `realloc()`).

Because snmalloc v2 no longer benefits from being provided the size of an allocated object (in, for example, dealloc), we may wish to adopt policies that allow objects to shrink in place beyond the lower bound of their sizeclass.

## Impact of Constraints On Deallocation

Previous editions of `snmalloc` stored metadata at "superslab" boundaries in the address space and relied on address arithmetic to map from small allocations to their associated metadata.
These operations relied on being able to take pointers out of bounds, and so posed challenges for `StrictProvenance` architectures.
The current edition of `snmalloc` instead follows pointers (starting from TLS or global roots), using address arithmetic only to derive indicies into these metadata pointers.

When the allocator client returns memory (or otherwise refers to an allocation), we will be careful to use the *lower bound* address, not the indicated address per se, for looking up the allocation.
The indicated address may be out of bounds, while `StrictProvenance` architectures should ensure that bounds are monotonically non-increasing, and so either

* the lower bound will always be within the original allocation.
* the pointer provided by the user will have zero length.

If we must detach an address from the pointer, as in deallocation, we will generally reject zero-length pointers, as if they were nullptr.

At the moment, **we permit a pointer to any part of an object to deallocate that object**.
`snmalloc`'s design ensures that we will not create a new, free "object" at an interior pointer but will, instead, always be able to find the beginning and end of the object in question.
In the future we are likely to be more strict, requiring authority to *the entire object* or at least *its lowest-address pointer-sized word* to free it.

## Object Lookup

`snmalloc` extends the traditional allocator interface with the `template<Boundary> void* external_pointer(void*)` family of functions, which generate additional pointers to live allocations.
To ensure that this function is not used as an amplification oracle, it must construct a return pointer with the same validity as its input even as it internally accesses metadata.
We make `external_pointer` use `pointer_offset` on the user-provided pointer, ensuring that the result has no more authority than the client already held.

# Adapting the Implementation

## Design Overview

For the majority of operations, no `StrictProvenance`-specific reasoning, beyond applying bounds, need be entertained.
However, as regions of memory move out of (and back into) the allocator's client and fast free lists, care must be taken to recover (and preserve) the internal, *vmem*-authorizing pointers from the user's much more tightly bounded pointers.

We store these internal pointers inside metadata, at different locations for each state:

* For free chunks in the "large `Range`" `Pipe`, we expect the `Range` objects themselves to work with these pointers.
  In practice, these ranges place these pointers in global state or the `Pagemap` `MetaEntry`s directly.

* Once outside the "large `Range`" `Pipe`, chunks holding heap objects will have a `SlabMetadata` structure associated with them, and we can store these high-authority pointers therein.
  (Specifically, the `StrictProvenanceBackendSlabMetadata` class adds an `arena` member to the `SlabMetadata` in use.)

* Metadata chunks themselves, however, do not have `SlabMetadata` structures and are managed using a "small `Range`" `Pipe`.
  These require special handling, considered below.

Within each (data) slab, there is (at least) one free list of objects.
We take the position that free list entries should be suitable for return, i.e., with authority bounded to their backing slab entry.
(However, the *contents* of free memory may be dangerous to expose to the user and require clearing prior to handing out.)

## Static Pointer Bound Taxonomy

We introduce a multi-dimensional space of bounds.  The facets are `enum class`-es in `snmalloc::capptr::dimension`.

* `Spatial` captures the intended spatial extent / role of the pointer: `Alloc`-ation, `Chunk`, or an entire `Arena`.

* `AddressSpaceControl` captures whether the pointer conveys control of its address space.

* `Wildness` captures whether the pointer has been checked to belong to this allocator.

These `dimension`s are composited using a `capptr::bound<>` type that we use as `B` in `CapPtr<T, B>`.
This is enforced (loosely) using the `IsBound` C++20 concept.

The namespace `snmalloc::capptr::bounds` contains particular points in the space of `capptr::bound<>` types:

* bounded to a large region of the address space with address space control, `Arena`;
* bounded to at least `MIN_CHUNK_SIZE` bytes with address space control, `Chunk`;
* bounded to at least `MIN_CHUNK_SIZE` bytes without address space control, `ChunkUser`;
* bounded to a smaller region but with address space control, `AllocFull`;
* bounded to a smaller region and without address space control, `Alloc`;
* unverified but presumed to be to an `Alloc`-ation, `AllocWild`.

## Primitive Architectural Operations

Several new functions are introduced to AALs to capture primitives of the Architecture.

* `CapPtr<T, Bout> capptr_bound(CapPtr<U, Bin> a, size_t sz)` spatially bounds the pointer `a` to have authority ranging only from its current target to its current target plus `sz` bytes (which must be within `a`'s authority).
  No imprecision in authority is permitted.
  The bounds annotations must obey `capptr_is_spatial_refinement`: the spatial dimension may change, but the others must be constant.

Ultimately, all address space manipulated by `snmalloc` comes from its Platform's primitive allocator.
An **arena** is a region returned by that provider.
The "large `Range`" `Pipe` serves to carve up `Arena`s; `Arena` pointers become `Chunk`s in the `backend`'s `alloc_chunk`.
`snmalloc`'s (new, as of `snmalloc2`) heap layouts ensure that metadata associated with any object are reachable through globals, meaning no explicit amplification is required.

## Primitive Platform Operations

* `CapPtr<void, Bout> capptr_to_user_address_control(CapPtr<T, Bin> f)` sheds authority over the address space from the `CapPtr`, on platforms where that is possible.
On CheriBSD, specifically, this strips the `VMAP` software permission, ensuring that clients cannot have the kernel manipulate heap pages.
The annotation `Bout` is *computed* as a function of `Bin`.
In future architectures, this is increasingly likely to be a no-op.

## Backend-Provided Operations

* `CapPtr<T, Bout> capptr_domesticate(LocalState *, CapPtr<T, Bin> ptr)` allows the backend to test whether `ptr` is sensible, by some definition thereof.
The annotation `Bout` is *computed* as a function of `Bin`.
`Bin` is required to be `Wild`, and `Bout` is `Tame` but otherwise identical.

## Constructed Operators

* `capptr_from_client` wraps a `void *` as a `capptr::AllocWild<void>` to mark it as unverified.

* `capptr_chunk_is_alloc` converts a `capptr::ChunkUser<T>` to a `capptr::Alloc<T>` without computational effect; it is intended to ease auditing.

* `capptr_reveal` converts a `capptr::Alloc<void>` to a `void*`, annotating where we mean to return a pointer to the user.

* `capptr_reveal_wild` converts a `capptr::AllocWild<void>` to a `void*`, annotating where we mean to return a *wild* pointer to the user (in `external_pointer`, e.g., where the result is just an offset of the user's pointer).

## Metadata Bounds Handling

We presently envision three policies for handling metadata:

1. Metadata kept within `snmalloc` is always `Arena`-bounded; metadata handed to the user (the `Allocator`s themselves) are exactly bounded as `Alloc`.
   Recycling internal metadata needs no amplification, and, as `Allocator`s are never deallocated, there is never a need to amplify an `Allocator*` back to a `Chunk`- or `Arena`-bounded pointer.

2. All metadata is exactly bounded as `Alloc`.
   Here, the "small `Range`" `Pipe` will require the ability to amplify back to `Chunk`- or `Arena`-bounded pointers when internal metadata is recycled.
   We believe this is straightforwardly possible using a "provenance capturing `Range`" above the outermost `Range` in the small `Range` `Pipe`.

3. No metadata is handed to the user; instead, opaque handles to `Allocator`s are given out.
   (CHERI sealed capabilities are an excellent candidate for such handles, but are beyond the scope of this document.)
   Here, it is no longer essential to bound any metadata pointers at all, though we may find it useful as a defence in depth.


**At the time of this writing, policy 1 is in effect**; pointers to `Allocator`s are bounded only at the periphery of `snmalloc`.

# Endnotes

[^mmu-perms]: Pointer authority generally *intersects* with MMU-based authorization.
For example, software using a pointer with both write and execute authority will still find that it cannot write to pages considered read-only by the MMU nor will it be able to execute non-executable pages.
Generally speaking, `snmalloc` requires only read-write access to memory it manages and merely passes through other permissions, with the exception of *vmem*, which it removes from any pointer it returns.

[^amplifier-state]: As we are largely following the fat pointer model and its evolution into CHERI capabilities, we achieve amplification through a *stateful*, *software* mechanism, rather than an architectural mechanism.
Specifically, the amplification mechanism will retain a superset of any authority it may be asked to reconstruct.
There have, in times past, been capability systems with architectural amplification (e.g., HYDRA's type-directed amplification), but we believe that future systems are unlikely to adopt this latter approach, necessitating the changes we propose below.

[^bounds-precision]: `StrictProvenance` architectures have historically differed in the precision with which authority can be represented.
Notably, it may not be possible to achieve byte-granular authority boundaries at every size scale.
In the case of CHERI specifically, `snmalloc`'s size classes and its alignment policies are already much coarser than existing architectural requirements for representable authority on all existing implementations.
