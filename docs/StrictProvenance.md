# StrictProvenance Architectures

By "strict (pointer) provenance", we mean that architectural mechanisms exist to make it impossible to use a pointer returned from `alloc()`/`malloc()` to access outside the allocation without also having some other pointer that already authorized those outside accesses.
CHERI is the canonical example we have in mind, but historically there have been other fat-pointer schemes with similar properties.

Borrowing terminology from CHERI, we speak of the **authority** to a region of address space held by a pointer and will justify actions in terms of this authority.
We may **bound** the authority of a pointer, deriving a new pointer with a subset of its progenitor's authority; this is assumed to be an ambient action requiring no additional authority.
Dually, we may **amplify** a pointer, constructing a pointer to the same place as another but with increased authority.[^amplifier-state]

[^amplifier-state] As we are largely following the fat pointer model and its evolution into CHERI capabilities, we achieve amplification through a *stateful*, *software* mechanism, rather than an architectural mechanism.
Specifically, the amplification mechanism will retain a superset of any authority it may be asked to reconstruct.
There have, in times past, been capability systems with architectural amplification (e.g., HYDRA's type-directed amplification), but we believe that future systems are unlikely to adopt this latter approach, necessitating the changes we propose below.

## Interaction with Future Temporal Extensions

We take the opportunity of refactoring to support `StrictProvenance` to envision `snmalloc`'s eventual, additional, interaction with memory coloring/versioning techniques such as Arm's MTE (in hybridization with CHERI).
Using `StrictProvenance` vocabulary and restricting attention to our envisioned use case, these hybridized techniques allow the creation of pointers that can have all their authority asynchronously invalidated.[^mte-cache]
We may therefore think of these pointers as *temporally bounded*, albeit with an upper bound chosen *a posteriori*.

[^mte-cache] Care will be required to ensure memory ordering of this change across cores.

# Sketching `snmalloc` on `StrictProvenance`

## Allocation

When allocating, we will need to ensure that returned pointers are bounded to no more than the slab entry used to back an allocation.
It may be useful, mostly for debugging, to more precisely bound returned pointers to the actual allocation size,[^bounds-precision] but this is not required for security.
(On sufficiently capable architectures, we will also prepare the returned pointer for temporal bounding.)

[^bounds-precision] `StrictProvenance` architectures have historically differed in the precision with which authority can be represented.
Notably, it may not be possible to achieve byte-granular authority boundaries at every size scale.
In the case of CHERI specifically, `snmalloc`'s size classes and its alignment policies are already much coarser than the architectural requirements for representable authority, and so it is always possible to bound returned pointers to their slab entry and in most cases more precise authority bounds are also representable.

`realloc()`-ation has several policies available.
We choose a fairly simple one for the moment: resizing in ways that do not change the backing allocation's `snmalloc` size class are left in place, while any change to the size class triggers an allocate-copy-deallocate sequence.
Even if `realloc()` leaves the object in place, the returned pointer should have its authority bounded as if this were a new allocation (and so may have less authority than `realloc()`'s pointer argument).
Notably, this policy is compatible with the existence of size-parameterized deallocation functions: the result of `realloc()` is always associated with the size class corresponding to the requested size.
(By contrast, shrinking in place in ways that changed the size class would require tracking the largest size associated with the allocation.)

For `realloc()`-ations left in place, we do not invalidate the original pointer and return one with equal temporal bounds (i.e., one that will be invalidated at the same time as the original).
For moving `realloc()`-ations, with their allocate-copy-deallocate implementation, the result will naturally be the revocation of the original pointer.
Clients of `realloc()` must not rely on the possible optimization of in-place operation and must not reuse the original pointer after passing it to `realloc()`.

## Deallocation

Strict provenance and bounded returns from `alloc()` imply that we cannot expect things like

```c++
void dealloc(void *p)
{
  Slab *slab = Slab::get(p);
  ... slab->foo ...
}
```

to work: `Slab::get()` is merely some pointer math, and so must either fail to construct its return value (e.g., by trapping) or construct a useless return value (e.g., one that traps on dereference).

On architectures with revocation, deallocation (and reallocation, naturally) must also check that its input pointer is still valid.
This will require some *atomic* work, to simultaneously mark the backing memory as free(ing) and prevent any other copy of this pointer from being used to deallocate the object.

## Object Lookup

`snmalloc` extends the traditional allocator interface with the `template<Boundary> void* external_pointer(void*)` family of functions, which generate additional pointers to live allocations.
To ensure that this function is not used as an amplification oracle, it must construct a return pointer with the same validity as its input even as it internally amplifies to access metadata.

XXX It may be worth requiring that the input pointer authorize the entire object?
What are the desired security properties here?

# Adapting the Implementation

## Design Overview

We will augment our `AddressSpaceManager` with a cache of pointers that span the entire heap managed by `snmalloc`.
To keep this cache small, we will request very large swaths (GiB-scale on >48-bit ASes) of address space at a time, even if we only populate those regions very slowly.
This cache will form the core of our amplification mechanism.

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
This means that allocation fast paths are unaffected by the requirement to bound return pointers, but that deallocation paths may need to amplify twice, once on receipt of the pointer from the application and again on receipt of the pointer from another `Alloc`.

## Pointer Wrapper Types

We introduce three type aliases for `void *`:

* `AuthPtr<T>`, which we use to mean pointers with elevated authority and pointing to arbitrary contents.
  They may be (effectively) unbounded, both spatially, by virtue of authorizing access to vast spans of heap, and temporally, in that they may be immune to invalidation.
  Because the indicated memory is arbitrary, it may not be sufficient to bound the pointer before returning it outside the allocator.

* `FreePtr<T>`, which captures pointers lingering on internal free lists.
  These are spatially bounded to one allocation and are temporally bounded to some as-yet-indeterminate point in the future.
  However, the contents of (a subset of) the memory they authorize may yet be unsafe to reveal to the client and may need to be cleared on allocation paths.

* `ReturnPtr`, which captures the pointers crossing the client interface.
  These are (spatially and temporally) bounded to just one, *live* allocation.
  They are created on the allocation paths from `AuthPtr` or `FreePtr` and are amplified back (and invalidated) on deallocation paths.
  The contents of authorized memory are arbitrary but safe for the client to (have) handled.
  The type is not parameterized because we intend it to be used only as `void *`.

## Primitive Architectural Operations

Several new functions are introduced to AALs to capture primitives of the Architecture:

* `FreePtr<T> ptrauth_bound(AuthPtr<T> a, size_t sz)` spatially bounds the pointer `a` to have authority ranging only from its current target to its current target plus `sz` bytes (which must be within `a`'s authority).
  No imprecision in authority is permitted.

  (Note that this method currently applies spatial bounds but not temporal bounds.)

* `AuthPtr<T> ptrauth_rebound(AuthPtr<T> a, ReturnPtr p)` is the *architectural primitive* enabling the software amplification mechanism.
  It combines the authority of `a` and the current target of `p`.
  The result may be safely dereferenced iff `a` authorizes access to `p`'s target; the result is temporally unbounded.
  The simplest sufficient (but not necessary) condition to ensure safety is that authority of `a` is a superset of the authority of `p` and `p` points within its authority.

## Amplification

The `AddressSpaceManager` now exposes a method with signature `AuthPtr<T> ptrauth_amplify(ReturnPtr p)` which uses `ptrauth_rebound` to construct a pointer targeting `p`'s target but bearing the authority of the primordial allocation granule (as provided by the kernel) containing this address.
This pointer can be used to reach the `Allocslab` metadata associated with `p` (and a good bit more, besides!).
