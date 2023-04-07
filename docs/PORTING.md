Porting snmalloc to a new platform
==================================

All of the platform-specific logic in snmalloc is isolated in the [Platform
Abstraction Layer (PAL)](../src/snmalloc/pal).
To add support for a new platform, you will need to implement a new PAL for
your system.

After version 0.5.2, the PAL is defined by a [C++20 Concept](../src/pal/pal_concept.h).
When compiling with C++20 or later, you should get helpful messages about any fields or methods that your PAL is missing.

The PAL must implement the following methods:

```c++
[[noreturn]] static void error(const char* const str) noexcept;
```
Report a fatal error and exit.

The memory that snmalloc is supplied from the Pal should be in one of three
states

* `using`
* `using readonly`
* `not using`

Before accessing the memory for a read, `snmalloc` will change the state to 
either `using` or `using readonly`,
and before a write by it will change the state to `using`.
If memory is not required any more, then `snmalloc` will change the state to
`not using`, and will ensure that it notifies the `Pal` again
before it every accesses that memory again.  
The `not using` state allows the `Pal` to recycle the memory for other purposes.
If `pal_enforce_access` is set as a mitigation, then accessing memory that has not been notified
correctly should trigger an exception/segfault.

The state for a particular region of memory is set with 
```c++
static void notify_not_using(void* p, size_t size) noexcept;

template<ZeroMem zero_mem>
static void notify_using(void* p, size_t size) noexcept;

static void notify_using_readonly(void* p, size_t size) noexcept;
```
These functions notify the system that the range of memory from `p` to `p` + 
`size` is in the relevant state.

If the template parameter is set to `YesZero` then `notify_using` must ensure
the range is full of zeros.

```c++
template<bool page_aligned = false>
static void zero(void* p, size_t size) noexcept;
```
Zero the range of memory from `p` to `p` + `size`.
This may be a simple `memset` call, but the `page_aligned` template parameter
allows for more efficient implementations when entire pages are being zeroed.
This function is typically called with very large ranges, so it may be more
efficient to request that the operating system provides background-zeroed
pages, rather than zeroing them synchronously in this call

```c++
template<bool state_using>
static void* reserve_aligned(size_t size) noexcept;
static void* reserve(size_t size) noexcept;
```
All platforms should provide `reserve` and can optionally provide
`reserve_aligned` if the underlying system can provide strongly aligned 
memory regions.
If the system guarantees only page alignment, implement only the second. `snmalloc` will
overallocate to ensure it can find suitably aligned blocks inside the region.
`reserve` should consider memory initially as `not_using`, and `snmalloc` will notify when it 
needs the range of memory.

If the system provides strong alignment, implement the first to return memory
at the desired alignment. If providing the first, then the `Pal` should also 
specify the minimum size block it can provide: 
```
static constexpr size_t minimum_alloc_size = ...;
```

The PAL is also responsible for advertising the page size:

```c++
static constexpr size_t page_size = 0x1000;
```

This is the granularity at which the PAL is able to mark memory as in-use or not-in-use.
The PAL is free to advertise a size greater than the minimum page size if that would be more efficient.
When a slab is deallocated, the PAL will be instructed to mark everything after the first `page_size` bytes as not-in-use and so larger values can lead to more memory overhead.

The page size for any given system depends on both the underlying architecture and the operating system.
The value exposed by the PAL may also depend on the Architecture Abstraction Layer (AAL).
For example, the Linux PAL advertises 64 KiB on PowerPC but 4 KiB on every other supported architecture:

```c++
static constexpr size_t page_size =
	Aal::aal_name == PowerPC ? 0x10000 : 0x1000;
```

Finally, you need to define a field to indicate the features that your PAL supports:
```c++
static constexpr uint64_t pal_features = ...;
```

These features are defined in the [`PalFeatures`](src/pal/pal_consts.h) enumeration.

There are several partial PALs that can be used when implementing POSIX-like systems:

 - `PALPOSIX` defines a PAL for a POSIX platform using no non-standard features.
 - `PALBSD` defines a PAL for the common set of BSD extensions to POSIX.
 - `PALBSD_Aligned` extends `PALBSD` to provide support for aligned allocation
   from `mmap`, as supported by NetBSD and FreeBSD.

Each of these template classes takes the PAL that inherits from it as a
template parameter.
A purely POSIX-compliant platform could have a PAL as simple as this:

```c++
class PALMyOS : public PALPOSIX<PALMyOS> {}
```

Typically, a PAL will implement at least one of the functions outlined above in
a more-efficient platform-specific way, but this is not required.
Non-POSIX systems will need to implement the entire PAL interface.
The [Windows](src/pal/pal_windows.h), and
[OpenEnclave](src/pal/pal_open_enclave.h) and
[FreeBSD kernel](src/pal/pal_freebsd_kernel.h) implementations give examples of
non-POSIX environments that snmalloc supports.

The POSIX PAL uses `mmap` to map memory.
Some POSIX or POSIX-like systems require minor tweaks to this behaviour.
Rather than requiring these to copy and paste the code, a PAL that inherits from the POSIX PAL can define one or both of these (`static constexpr`) fields to customise the `mmap` behaviour.

 - `default_mmap_flags` allows a PAL to provide additional `MAP_*`
    flags to all `mmap` calls.
 - `anonymous_memory_fd` allows the PAL to override the default file
   descriptor used for memory mappings.

