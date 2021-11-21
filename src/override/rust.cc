#define SNMALLOC_NAME_MANGLE(a) sn_##a
#include "malloc.cc"

#include <cstring>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

using namespace snmalloc;

namespace
{
  enum class SizeChange
  {
    Unknown,
    Grow,
    Shrink
  };
  template<ZeroMem zero_mem, SizeChange size_change>
  SNMALLOC_FAST_PATH_INLINE void* realloc_helper(
    Alloc& alloc,
    void* ptr,
    size_t old_alignment,
    size_t old_size,
    size_t new_alignment,
    size_t new_size)
  {
    size_t aligned_old_size = aligned_size(old_alignment, old_size),
           aligned_new_size = aligned_size(new_alignment, new_size);
    if (
      size_to_sizeclass_full(aligned_old_size).raw() ==
      size_to_sizeclass_full(aligned_new_size).raw())
      return ptr;
    // this may memset more than it needs, but doing so increases
    // the possibility for large area to be efficiently zeroed by special
    // methods
    void* p = alloc.alloc<zero_mem>(aligned_new_size);
    if (p)
    {
      if constexpr (size_change == SizeChange::Unknown)
        std::memcpy(p, ptr, old_size < new_size ? old_size : new_size);
      else if constexpr (size_change == SizeChange::Grow)
        std::memcpy(p, ptr, old_size);
      else
        std::memcpy(p, ptr, new_size);
      alloc.dealloc(ptr, aligned_old_size);
    }
    return p;
  }
}

extern "C"
{
  /*
   * Global allocator
   */
  SNMALLOC_EXPORT void*
    SNMALLOC_NAME_MANGLE(rust_alloc)(size_t alignment, size_t size)
  {
    return ThreadAlloc::get().alloc(aligned_size(alignment, size));
  }

  SNMALLOC_EXPORT void*
    SNMALLOC_NAME_MANGLE(rust_alloc_zeroed)(size_t alignment, size_t size)
  {
    return ThreadAlloc::get().alloc<YesZero>(aligned_size(alignment, size));
  }

  SNMALLOC_EXPORT void
    SNMALLOC_NAME_MANGLE(rust_dealloc)(void* ptr, size_t alignment, size_t size)
  {
    ThreadAlloc::get().dealloc(ptr, aligned_size(alignment, size));
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(rust_realloc)(
    void* ptr, size_t alignment, size_t old_size, size_t new_size)
  {
    return realloc_helper<NoZero, SizeChange::Unknown>(
      ThreadAlloc::get(), ptr, alignment, old_size, alignment, new_size);
  }

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(rust_statistics)(
    size_t* current_memory_usage, size_t* peak_memory_usage)
  {
    auto unused_chunks = Globals::get_chunk_allocator_state().unused_memory();
    auto peak = Globals::get_chunk_allocator_state().peak_memory_usage();
    *current_memory_usage = peak - unused_chunks;
    *peak_memory_usage = peak;
  }

  /*
   * Custom heap allocator (for `allocator_api`)
   * TODO: consider fallible allocation
   */

  SNMALLOC_EXPORT Alloc* SNMALLOC_NAME_MANGLE(rust_allocator_new)(void)
  {
    auto scope_allocator = ScopedAllocator{};
    auto alloc = new (scope_allocator->alloc<sizeof(Alloc)>()) Alloc;
    alloc->init();
    return alloc;
  }

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(rust_allocator_drop)(Alloc* alloc)
  {
    auto scope_allocator = ScopedAllocator{};
    alloc->teardown();
    scope_allocator->dealloc<sizeof(Alloc)>(alloc);
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(rust_allocator_allocate)(
    Alloc* alloc, size_t alignment, size_t size)
  {
    return alloc->alloc(aligned_size(alignment, size));
  }

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(rust_allocator_deallocate)(
    Alloc* alloc, void* ptr, size_t alignment, size_t size)
  {
    SNMALLOC_ASSUME(nullptr != ptr);
    alloc->dealloc(ptr, aligned_size(alignment, size));
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(rust_allocator_allocate_zeroed)(
    Alloc* alloc, size_t alignment, size_t size)
  {
    return alloc->alloc<YesZero>(aligned_size(alignment, size));
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(rust_allocator_grow)(
    Alloc* alloc,
    void* ptr,
    size_t old_alignment,
    size_t old_size,
    size_t new_alignment,
    size_t new_size)
  {
    SNMALLOC_ASSUME(nullptr != ptr);
    SNMALLOC_ASSUME(new_size >= old_size);
    return realloc_helper<NoZero, SizeChange::Grow>(
      *alloc, ptr, old_alignment, old_size, new_alignment, new_size);
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(rust_allocator_grow_zeroed)(
    Alloc* alloc,
    void* ptr,
    size_t old_alignment,
    size_t old_size,
    size_t new_alignment,
    size_t new_size)
  {
    SNMALLOC_ASSUME(nullptr != ptr);
    SNMALLOC_ASSUME(new_size >= old_size);
    return realloc_helper<YesZero, SizeChange::Grow>(
      *alloc, ptr, old_alignment, old_size, new_alignment, new_size);
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(rust_allocator_shrink)(
    Alloc* alloc,
    void* ptr,
    size_t old_alignment,
    size_t old_size,
    size_t new_alignment,
    size_t new_size)
  {
    SNMALLOC_ASSUME(nullptr != ptr);
    SNMALLOC_ASSUME(new_size <= old_size);
    return realloc_helper<NoZero, SizeChange::Shrink>(
      *alloc, ptr, old_alignment, old_size, new_alignment, new_size);
  }

  SNMALLOC_EXPORT bool SNMALLOC_NAME_MANGLE(rust_allocator_fit_inplace)(
    size_t old_alignment,
    size_t old_size,
    size_t new_alignment,
    size_t new_size)
  {
    size_t aligned_old_size = aligned_size(old_alignment, old_size),
           aligned_new_size = aligned_size(new_alignment, new_size);
    return size_to_sizeclass_full(aligned_old_size).raw() ==
      size_to_sizeclass_full(aligned_new_size).raw();
  }

  // This function is to calculate the excessive size.
  SNMALLOC_EXPORT
  size_t SNMALLOC_NAME_MANGLE(rust_round_size)(size_t alignment, size_t size)
  {
    return round_size(aligned_size(alignment, size));
  }
}
