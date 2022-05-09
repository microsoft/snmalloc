# Randomisation

The relative allocation pattern of objects can also be used to increase the power of an exploit.
This is a weak defence as spraying can defeat pretty much any randomisation, so this is just a case of doing enough to raise the bar.

There are three things we randomise about the allocation pattern in snmalloc:

* Initial order of allocations on a slab
* Subsequent order of allocations on a slab
* When we consume all allocations on a slab

## Initial slab order

We build the initial order of allocation using a classic algorithm for building a permutation of a set.
When I started writing this code, I remembered my undergraduate lectures on creating a permutation using a Fisher–Yates shuffle.
Unfortunately, I couldn't find my very old notes, so I had to use Wikipedia to refresh my knowledge.

After reading Wikipedia I realised, I actually wanted Sattolo's algorithm for generating a cyclic permutation using the "inside-out" algorithm.
This algorithm builds a cyclic permutation of a set, which is exactly what we need to build all possible free lists.
Using the "inside-out" algorithm gives much better cache performance.

The algorithm is:
```C++
  object[0].next = &(object[0]); // 1 element cycle
  for (i = 1; i < n; i++)
  {
    auto j = random(0, i-1);     // Pick element in cycle
    // Cyclic list insert of i after j
    object[i].next = object[j].next;
    object[j].next = &(object[i]);
  }
  auto end_index = random(0,n-1);      // Select last element of cycle.
  auto start = object[end_index].next; // Find start
  object[end_index].next = nullptr;    // Terminate list
```
When this completes you are guaranteed that `start` will be a list where next takes you through all the other elements.

Now, to generate all possible free lists with equal probabilty `random` has to be a uniform distribution, but that is prohibitively expensive.
Here we cut a corner and approximate the distribution for performance.

Another complexity is that to build the protected free list from the previous blog post, we actually require a second pass over this list as we cannot build the back edges until we know the order of the list.

## Preserving randomness

We have an amazing amount of randomness within a slab, but that becomes predictable if we can't introduce more entropy as the system runs.
To address this, we actually build pairs of free-queue for each slab.

Each slab has two free-queues, when we deallocate an object we use a cheap coin flip to decide which queue to add the element to.
When we want a new free-queue to start allocating from, we take the longer of the free-queues from the meta-data and use that in our thread local allocator.

## Almost full slabs

Now the two randomisations above make relative addresses hard to guess, but those alone do not prevent it being easy to predict when a slab will be full.
We use two mechanisms to handle this

* Only consider a slab for reuse when it has a certain percentage of free elements
* If there is a single slab that can currently be used, use a random coin flip to decide whether we allocate a new slab instead of using the existing slab.

These two mechanisms are aimed at making it hard to allocate an object that is with high probability adjacent to another allocated object.
This is important for using the free-queue protection to catch various corruptions.


## Improving protection

Now the free-queue protection with randomisation will make exploits considerably harder, but it will not catch all corruptions.
We have been working on adding support for both CHERI and memory tagging to snmalloc, which are more comprehensive defences to memory corruption.
Our aim with the hardening of snmalloc has been to provide something that can be always on in production.

[Now, we have explained the various hardening concepts, you are better placed to judge the performance we achieve.](./README.md)
