#pragma once

#include "../ds/bits.h"
#include "../pal/pal.h"

namespace snmalloc
{
// The CHECK_CLIENT macro is used to turn on minimal checking of the client
// calling the API correctly.
#if !defined(NDEBUG) && !defined(CHECK_CLIENT)
#  define CHECK_CLIENT
#endif

  inline SNMALLOC_FAST_PATH void
  check_client_impl(bool test, const char* const str)
  {
#ifdef CHECK_CLIENT
    if (unlikely(!test))
      error(str);
#else
    UNUSED(test);
    UNUSED(str);
#endif
  }
#ifdef CHECK_CLIENT
#  define check_client(test, str) check_client_impl(test, str)
#else
#  define check_client(test, str)
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

  // Minimum slab size.
  static constexpr size_t MIN_CHUNK_BITS = 14;
  static constexpr size_t MIN_CHUNK_SIZE = bits::one_at_bit(MIN_CHUNK_BITS);

  // Minimum number of objects on a slab
#ifdef CHECK_CLIENT
  static constexpr size_t MIN_OBJECT_COUNT = 13;
#else
  static constexpr size_t MIN_OBJECT_COUNT = 4;
#endif


  // Maximum size of an object that uses sizeclasses.
  static constexpr size_t MAX_SIZECLASS_BITS = 16;
  static constexpr size_t MAX_SIZECLASS_SIZE =
    bits::one_at_bit(MAX_SIZECLASS_BITS);

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
} // namespace snmalloc
