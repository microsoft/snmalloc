# Difference from published paper

This document outlines the changes that have diverged from
[the published paper](snmalloc.pdf) on `snmalloc`.

  1.  Link no longer terminates the bump-free list.  The paper describes a 
      complex invariant for how the final element of the bump-free list can 
      also be the link node.

      We now have a much simpler invariant.  The link is either 1, signifying 
      the block is completely full.  Or not 1, signifying it has at least one
      free element at the offset contained in link, and that contains the DLL
      node for this sizeclass.

      The bump-free list contains additional free elements, and the remaining
      bump allocated space.

      The value 1, is never a valid bump allocation value, as we initially
      allocate the first entry as the link, so we can use 1 as the no more bump
      space value. 

  2.  Separate Bump/Free list.  We have separate bump ptr and free list.  This 
      is required to have a "fast free list" in each allocator for each
      sizeclass.  We bump allocate a whole os page (4KiB) worth of allocations
      in one go, so that the CPU predicts the free list path for the fast 
      path.

  3.  Per allocator per sizeclass fast free list.  Each allocator has an array
      for each small size class that contains a free list of some elements for 
      that sizeclass. This enables a very compressed path for the common 
      allocation case.

  4.  We now store a direct pointer to the next element in each slabs free list
      rather than a relative offset into the slab.  This enables list
      calculation on the fast path.
 
  5.  There is a single bump-ptr per size class that is part of the
      allocator structure.  The per size class slab list now only contains slabs
      with free list, and not if it only has a bump ptr.

[2-4]  Are changes that are directly inspired by
(mimalloc)[http://github.com/microsoft/mimalloc].