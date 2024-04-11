#pragma once

#include "../backend/globalconfig.h"
#include "../ds_core/ds_core.h"

namespace snmalloc
{
  template<typename T = MainPartition>
  SNMALLOC_FAST_PATH_INLINE LocalAllocator<Alloc::Config, T>& get_alloc()
  {
#ifdef SNMALLOC_EXTERNAL_THREAD_ALLOC
    static_assert(
      std::is_same_v<T, MainPartition>,
      "SNMALLOC_EXTERNAL_THREAD_ALLOC only supports the default allocator.");
    return ThreadAllocExternal::get();
#else
    return ThreadLocal<LocalAllocator<Alloc::Config, T>>::get();
#endif
  }
} // namespace snmalloc