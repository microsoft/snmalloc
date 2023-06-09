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
#if defined(SNMALLOC_QEMU_WORKAROUND) && defined(SNMALLOC_VA_BITS_64)
  /*
   * QEMU user-mode, up through and including v7.2.0-rc4, the latest tag at the
   * time of this writing, does not use a tree of any sort to store its opinion
   * of the address space, allocating an amount of memory linear in the size of
   * any created map, not the number of pages actually used.  This is
   * exacerbated in and after qemu v6 (or, more specifically, d9c58585), which
   * grew the proportionality constant.
   *
   * In any case, for our CI jobs, then, use a larger minimum chunk size (that
   * is, pagemap granularity) than by default to reduce the size of the
   * pagemap.  We can't raise this *too* much, lest we hit constexpr step
   * limits in the sizeclasstable magic!  17 bits seems to be the sweet spot
   * and means that any of our tests can run in a little under 2 GiB of RSS
   * even on QEMU versions after v6.
   */
  static constexpr size_t MIN_CHUNK_BITS = static_cast<size_t>(17);
#else
  static constexpr size_t MIN_CHUNK_BITS = static_cast<size_t>(14);
#endif
  static constexpr size_t MIN_CHUNK_SIZE = bits::one_at_bit(MIN_CHUNK_BITS);

  // Minimum number of objects on a slab
  static constexpr size_t MIN_OBJECT_COUNT =
    mitigations(random_larger_thresholds) ? 13 : 4;

  // Maximum size of an object that uses sizeclasses.
#if defined(SNMALLOC_QEMU_WORKAROUND) && defined(SNMALLOC_VA_BITS_64)
  /*
   * As a consequence of our significantly larger minimum chunk size, we need
   * to raise the threshold for what constitutes a large object (which must
   * be a multiple of the minimum chunk size).  Extend the space of small
   * objects up enough to match yet preserve the notion that there exist small
   * objects larger than MIN_CHUNK_SIZE.
   */
  static constexpr size_t MAX_SMALL_SIZECLASS_BITS = 19;
#else
  static constexpr size_t MAX_SMALL_SIZECLASS_BITS = 16;
#endif
  static constexpr size_t MAX_SMALL_SIZECLASS_SIZE =
    bits::one_at_bit(MAX_SMALL_SIZECLASS_BITS);

  static_assert(
    MAX_SMALL_SIZECLASS_SIZE >= MIN_CHUNK_SIZE,
    "Large sizes need to be representable by as a multiple of MIN_CHUNK_SIZE");

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

  // Used to configure when the backend should use thread local buddies.
  // This only basically is used to disable some buddy allocators on small
  // fixed heap scenarios like OpenEnclave.
  static constexpr size_t MIN_HEAP_SIZE_FOR_THREAD_LOCAL_BUDDY =
    bits::one_at_bit(27);
} // namespace snmalloc
