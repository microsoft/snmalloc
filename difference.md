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
