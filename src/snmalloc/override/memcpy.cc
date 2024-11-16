#include "override.h"

extern "C"
{
  /**
   * Snmalloc checked memcpy.
   */
  SNMALLOC_EXPORT void*
  SNMALLOC_NAME_MANGLE(memcpy)(void* dst, const void* src, size_t len)
  {
    return snmalloc::memcpy<true>(dst, src, len);
  }

  /**
   * Snmalloc checked memmove.
   */
  SNMALLOC_EXPORT void*
  SNMALLOC_NAME_MANGLE(memmove)(void* dst, const void* src, size_t len)
  {
    return snmalloc::memmove<true>(dst, src, len);
  }
}
