#pragma once

namespace snmalloc
{
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

  // Minimum allocation size is space for two pointers.
  static_assert(bits::next_pow2_const(sizeof(void*)) == sizeof(void*));
  static constexpr size_t MIN_ALLOC_SIZE = 2 * sizeof(void*);
  static constexpr size_t MIN_ALLOC_BITS = bits::ctz_const(MIN_ALLOC_SIZE);

  // Minimum slab size.
  static constexpr size_t MIN_CHUNK_BITS = bits::max(
    static_cast<size_t>(14), bits::next_pow2_bits_const(OS_PAGE_SIZE));
  static constexpr size_t MIN_CHUNK_SIZE = bits::one_at_bit(MIN_CHUNK_BITS);

  // Minimum number of objects on a slab
#ifdef SNMALLOC_CHECK_CLIENT
  static constexpr size_t MIN_OBJECT_COUNT = 13;
#else
  static constexpr size_t MIN_OBJECT_COUNT = 4;
#endif

  // Maximum size of an object that uses sizeclasses.
  static constexpr size_t MAX_SMALL_SIZECLASS_BITS = 16;
  static constexpr size_t MAX_SMALL_SIZECLASS_SIZE =
    bits::one_at_bit(MAX_SMALL_SIZECLASS_BITS);

  // Number of slots for remote deallocation.
  static constexpr size_t REMOTE_SLOT_BITS = 8;
  static constexpr size_t REMOTE_SLOTS = 1 << REMOTE_SLOT_BITS;
  static constexpr size_t REMOTE_MASK = REMOTE_SLOTS - 1;

  static_assert(
    INTERMEDIATE_BITS < MIN_ALLOC_BITS,
    "INTERMEDIATE_BITS must be less than MIN_ALLOC_BITS");
  static_assert(
    MIN_ALLOC_SIZE >= (sizeof(void*) * 2),
    "MIN_ALLOC_SIZE must be sufficient for two pointers");

  // Return remote small allocs when the local cache reaches this size.
  static constexpr int64_t REMOTE_CACHE =
#ifdef USE_REMOTE_CACHE
    USE_REMOTE_CACHE
#else
    1 << MIN_CHUNK_BITS
#endif
    ;
} // namespace snmalloc
