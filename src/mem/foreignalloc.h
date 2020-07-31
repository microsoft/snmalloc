#pragma once

extern "C"
{
  /**
   * A closure telling us how to free back to a foreign allocator.
   *
   * Any security-domain crossing will be encapsulated inside `->free()` here.
   *
   * Because our OOBMap is quite coarse-grained (to make up for the fact that
   * it's storing pointers) relative to the ChunkMap, the `->free()` function
   * here could internally dispatch on the address to route to one of many
   * sandboxes within the OOBMap granule, assuming that more than one fit.  If
   * this becomes common, we should adjust the interface here to either fix the
   * type of `arg` or expose another function pointer to associate at a finer
   * scale.
   */

  struct ForeignAllocator
  {
    void (*free)(void* arg, void* p);
    void* arg;
  };
}
