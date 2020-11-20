#pragma once

#include "../ds/bits.h"

namespace snmalloc
{
// The CHECK_CLIENT macro is used to turn on minimal checking of the client
// calling the API correctly.
#if !defined(NDEBUG) && !defined(CHECK_CLIENT)
#  define CHECK_CLIENT
#endif

  // 0 intermediate bits results in power of 2 small allocs. 1 intermediate
  // bit gives additional sizeclasses at the midpoint between each power of 2.
  // 2 intermediate bits gives 3 intermediate sizeclasses, etc.
  static constexpr size_t INTERMEDIATE_BITS =
#ifdef USE_INTERMEDIATE_BITS
    USE_INTERMEDIATE_BITS
#else
    2
#endif
    ;

  // Return remote small allocs when the local cache reaches this size.
  static constexpr int64_t REMOTE_CACHE =
#ifdef USE_REMOTE_CACHE
    USE_REMOTE_CACHE
#else
    1 << 20
#endif
    ;

  // Handle at most this many object from the remote dealloc queue at a time.
  static constexpr size_t REMOTE_BATCH =
#ifdef USE_REMOTE_BATCH
    REMOTE_BATCH
#else
    4096
#endif
    ;

  // Specifies smaller slab and super slab sizes for address space
  // constrained scenarios.
  static constexpr size_t USE_LARGE_CHUNKS =
#ifdef SNMALLOC_USE_LARGE_CHUNKS
    // In 32 bit uses smaller superslab.
    (bits::is64())
#else
    false
#endif
    ;

  // Specifies even smaller slab and super slab sizes for open enclave.
  static constexpr size_t USE_SMALL_CHUNKS =
#ifdef SNMALLOC_USE_SMALL_CHUNKS
    true
#else
    false
#endif
    ;

  enum DecommitStrategy
  {
    /**
     * Never decommit memory.
     */
    DecommitNone,
    /**
     * Decommit superslabs when they are entirely empty.
     */
    DecommitSuper,
    /**
     * Decommit superslabs only when we are informed of memory pressure by the
     * OS, do not decommit anything in normal operation.
     */
    DecommitSuperLazy
  };

  static constexpr DecommitStrategy decommit_strategy =
#ifdef USE_DECOMMIT_STRATEGY
    USE_DECOMMIT_STRATEGY
#elif defined(_WIN32) && !defined(OPEN_ENCLAVE)
    DecommitSuperLazy
#else
    DecommitSuper
#endif
    ;

  // The remaining values are derived, not configurable.
  static constexpr size_t POINTER_BITS =
    bits::next_pow2_bits_const(sizeof(uintptr_t));

  // Used to isolate values on cache lines to prevent false sharing.
  static constexpr size_t CACHELINE_SIZE = 64;

  static constexpr size_t PAGE_ALIGNED_SIZE = OS_PAGE_SIZE << INTERMEDIATE_BITS;

  // Minimum allocation size is space for two pointers.
  static_assert(bits::next_pow2_const(sizeof(void*)) == sizeof(void*));
  static constexpr size_t MIN_ALLOC_SIZE = 2 * sizeof(void*);
  static constexpr size_t MIN_ALLOC_BITS = bits::ctz_const(MIN_ALLOC_SIZE);

  // Slabs are 64 KiB unless constrained to 16 or even 8 KiB
  static constexpr size_t SLAB_BITS =
    USE_SMALL_CHUNKS ? 13 : (USE_LARGE_CHUNKS ? 16 : 14);
  static constexpr size_t SLAB_SIZE = 1 << SLAB_BITS;
  static constexpr size_t SLAB_MASK = ~(SLAB_SIZE - 1);

  // Superslabs are composed of this many slabs. Slab offsets are encoded as
  // a byte, so the maximum count is 256. This must be a power of two to
  // allow fast masking to find a superslab start address.
  static constexpr size_t SLAB_COUNT_BITS =
    USE_SMALL_CHUNKS ? 5 : (USE_LARGE_CHUNKS ? 8 : 6);
  static constexpr size_t SLAB_COUNT = 1 << SLAB_COUNT_BITS;
  static constexpr size_t SUPERSLAB_SIZE = SLAB_SIZE * SLAB_COUNT;
  static constexpr size_t SUPERSLAB_MASK = ~(SUPERSLAB_SIZE - 1);
  static constexpr size_t SUPERSLAB_BITS = SLAB_BITS + SLAB_COUNT_BITS;

  static_assert((1ULL << SUPERSLAB_BITS) == SUPERSLAB_SIZE, "Sanity check");

  // Number of slots for remote deallocation.
  static constexpr size_t REMOTE_SLOT_BITS = 6;
  static constexpr size_t REMOTE_SLOTS = 1 << REMOTE_SLOT_BITS;
  static constexpr size_t REMOTE_MASK = REMOTE_SLOTS - 1;

  static_assert(
    INTERMEDIATE_BITS < MIN_ALLOC_BITS,
    "INTERMEDIATE_BITS must be less than MIN_ALLOC_BITS");
  static_assert(
    MIN_ALLOC_SIZE >= (sizeof(void*) * 2),
    "MIN_ALLOC_SIZE must be sufficient for two pointers");
  static_assert(
    SLAB_BITS <= (sizeof(uint16_t) * 8),
    "SLAB_BITS must not be more than the bits in a uint16_t");
  static_assert(
    SLAB_COUNT == bits::next_pow2_const(SLAB_COUNT),
    "SLAB_COUNT must be a power of 2");
  static_assert(
    SLAB_COUNT <= (UINT8_MAX + 1), "SLAB_COUNT must fit in a uint8_t");
} // namespace snmalloc
