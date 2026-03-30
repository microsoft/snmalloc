#pragma once

#include "../ds_core/sizeclassconfig.h"
#include "mitigations.h"

namespace snmalloc
{
  // Minimum number of objects on a slab
  static constexpr size_t MIN_OBJECT_COUNT =
    mitigations(random_larger_thresholds) ? 13 : 4;

  /**
   * The number of bits needed to count the number of objects within a slab.
   *
   * Most likely, this is achieved by the smallest sizeclass, which will have
   * many more than MIN_OBJECT_COUNT objects in its slab.  But, just in case,
   * it's defined here and checked when we compute the sizeclass table, since
   * computing this number is potentially nontrivial.
   */
#if defined(SNMALLOC_QEMU_WORKAROUND) && defined(SNMALLOC_VA_BITS_64)
  static constexpr size_t MAX_CAPACITY_BITS = 13;
#else
  static constexpr size_t MAX_CAPACITY_BITS = 11;
#endif

  /**
   * The maximum distance between the start of two objects in the same slab.
   */
  static constexpr size_t MAX_SLAB_SPAN_SIZE =
    (MIN_OBJECT_COUNT - 1) * MAX_SMALL_SIZECLASS_SIZE;
  static constexpr size_t MAX_SLAB_SPAN_BITS =
    bits::next_pow2_bits_const(MAX_SLAB_SPAN_SIZE);

  // Number of slots for remote deallocation.
  static constexpr size_t REMOTE_SLOT_BITS = 8;
  static constexpr size_t REMOTE_SLOTS = 1 << REMOTE_SLOT_BITS;
  static constexpr size_t REMOTE_MASK = REMOTE_SLOTS - 1;

#if defined(SNMALLOC_DEALLOC_BATCH_RING_ASSOC)
  static constexpr size_t DEALLOC_BATCH_RING_ASSOC =
    SNMALLOC_DEALLOC_BATCH_RING_ASSOC;
#else
#  if defined(__has_cpp_attribute)
#    if ( \
      __has_cpp_attribute(msvc::no_unique_address) && \
      (__cplusplus >= 201803L || _MSVC_LANG >= 201803L)) || \
      __has_cpp_attribute(no_unique_address)
  // For C++20 or later, we do have [[no_unique_address]] and so can also do
  // batching if we aren't turning on the backward-pointer mitigations
  static constexpr size_t DEALLOC_BATCH_MIN_ALLOC_WORDS =
    mitigations(freelist_backward_edge) ? 4 : 2;
#    else
  // For C++17, we don't have [[no_unique_address]] and so we always end up
  // needing all four pointers' worth of space (because BatchedRemoteMessage has
  // two freelist::Object::T<> links within, each of which will have two fields
  // and will be padded to two pointers).
  static constexpr size_t DEALLOC_BATCH_MIN_ALLOC_WORDS = 4;
#    endif
#  else
  // If we don't even have the feature test macro, we're C++17 or earlier.
  static constexpr size_t DEALLOC_BATCH_MIN_ALLOC_WORDS = 4;
#  endif

  static constexpr size_t DEALLOC_BATCH_RING_ASSOC =
    (MIN_ALLOC_SIZE >= (DEALLOC_BATCH_MIN_ALLOC_WORDS * sizeof(void*))) ? 2 : 0;
#endif

#if defined(SNMALLOC_DEALLOC_BATCH_RING_SET_BITS)
  static constexpr size_t DEALLOC_BATCH_RING_SET_BITS =
    SNMALLOC_DEALLOC_BATCH_RING_SET_BITS;
#else
  static constexpr size_t DEALLOC_BATCH_RING_SET_BITS = 3;
#endif

  static constexpr size_t DEALLOC_BATCH_RINGS =
    DEALLOC_BATCH_RING_ASSOC * bits::one_at_bit(DEALLOC_BATCH_RING_SET_BITS);

  // Return remote small allocs when the local cache reaches this size.
  static constexpr int64_t REMOTE_CACHE =
#ifdef USE_REMOTE_CACHE
    USE_REMOTE_CACHE
#else
    MIN_CHUNK_SIZE
#endif
    ;

  // Stop processing remote batch when we reach this amount of deallocations in
  // bytes
  static constexpr int64_t REMOTE_BATCH_LIMIT =
#ifdef SNMALLOC_REMOTE_BATCH_PROCESS_SIZE
    SNMALLOC_REMOTE_BATCH_PROCESS_SIZE
#else
    1 * 1024 * 1024
#endif
    ;

  // Used to configure when the backend should use thread local buddies.
  // This only basically is used to disable some buddy allocators on small
  // fixed heap scenarios like OpenEnclave.
  static constexpr size_t MIN_HEAP_SIZE_FOR_THREAD_LOCAL_BUDDY =
    bits::one_at_bit(27);
} // namespace snmalloc
