# How Objects Come Home

`snmalloc` returns freed objects back to the front-end allocator whence they came.
Let us follow the path of an object from `dealloc` back to its home slab's free list.
Of particular interest is the staged repurposing of the linkage pointers within free objects.

1. `LocalAlloc::dealloc` probes the page map and detects that
   the `RemoteAllocator` owning the home slab is not the current `LocalAlloc`.

2. `LocalAlloc::dealloc` invokes `RemoteDeallocCache::reserve_space`,
   which checks whether the current allocator's `RemoteDeallocCache` is over quota.

3. Regardless of the outcome, we call `RemoteDeallocCache::dealloc`,
   either in `LocalAlloc::dealloc` itself or in `LocalAlloc::dealloc_remote_slow`.

   At this point, the object is cast to a `freelist::Object::T<>`,
   which uses the top word (or two) to hold a forward pointer and,
   if mitigations are enabled, an obfuscated backward pointer (for checking).

   Here, the `freelist::Object::T<>` linkages are used by `RemoteDeallocCache`.

4. Assuming, for exposition, that we would now be over quota,
   we enter `LocalAlloc::dealloc_remote_slow` which calls `RemoteDeallocCache::post` 
   (indirectly, via a few wrappers).
   This latter function is responsible for calling `RemoteAllocator::enqueue`,
   which moves each list to the message queue of the allocator at the front.

   In this distributing, the list that would be associated with the current allocator
   is instead sharded out to other lists and distribution is repeated.
   (However, the current allocator will never be the owner of any message in this list;
   the repeated distribution is to ensure that objects return home in finite steps.)
   The sharding is done in such a way as to guarantee termination of this loop.

   After `RemoteAllocator::enqueue`,
   the `freelist::Object::T<>` linkages are used by the `RemoteAllocator` message queue.

5. Eventually, a remote allocator invokes `CoreAlloc::handle_message_queue`,
   which chains to `CoreAlloc::handle_message_queue_inner` when incoming messages exist.
   This invokes `CoreAlloc::handle_dealloc_remote` for each message in the queue.

6. `CoreAlloc::handle_dealloc_remote` replicates the test performed in `LocalAlloc::dealloc`
   to determine whether an object is owned by the invoking allocator or another.
   In the former case, the object is returned to the appropriate slab;
   in the latter case, it is pushed into the invoking allocator's remote cache,
   akin to the handling in `LocalAlloc::dealloc_remote_slow`
   (with the slight tweak that draining the local cache, if necessary,
   will be deferred until the entire message queue has been processed).

7. After potentially many hops between allocators,
   the object will arrive home and will be returned to its slab.

   When that happens, the `freelist::Object::T<>` linkages are used by the slab
   for its free list(s).
