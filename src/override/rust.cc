#include "../mem/slowalloc.h"
#include "../snmalloc.h"

#include <cstring>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

using namespace snmalloc;

extern "C" SNMALLOC_EXPORT void* rust_alloc(size_t alignment, size_t size)
{
  return ThreadAlloc::get_noncachable()->alloc(aligned_size(alignment, size));
}

extern "C" SNMALLOC_EXPORT void
rust_dealloc(void* ptr, size_t alignment, size_t size)
{
  ThreadAlloc::get_noncachable()->dealloc(ptr, aligned_size(alignment, size));
}

extern "C" SNMALLOC_EXPORT void*
rust_realloc(void* ptr, size_t alignment, size_t old_size, size_t new_size)
{
  size_t aligned_old_size = aligned_size(alignment, old_size),
         aligned_new_size = aligned_size(alignment, new_size);
  if (
    size_to_sizeclass(aligned_old_size) == size_to_sizeclass(aligned_new_size))
    return ptr;
  void* p = ThreadAlloc::get_noncachable()->alloc(aligned_new_size);
  if (p)
  {
    std::memcpy(p, ptr, old_size < new_size ? old_size : new_size);
    ThreadAlloc::get_noncachable()->dealloc(ptr, aligned_old_size);
  }
  return p;
}
