#pragma once

#include "../backend/globalconfig.h"
#include "../ds_core/ds_core.h"

namespace snmalloc
{
  SNMALLOC_FAST_PATH_INLINE Alloc& get_alloc()
  {
#ifdef SNMALLOC_EXTERNAL_THREAD_ALLOC
    return ThreadAllocExternal::get();
#else
    return ThreadLocal<Alloc>::get();
#endif
  }
} // namespace snmalloc