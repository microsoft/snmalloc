#pragma once

#include "snmalloc/ds_core/defines.h"
#include "snmalloc/ds_core/ptrwrap.h"

#ifdef SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION
#  include "snmalloc/mem/secondary/gwp_asan.h"

namespace snmalloc
{
  using SecondaryAllocator = GwpAsanSecondaryAllocator;
} // namespace snmalloc
#else
#  include "snmalloc/mem/secondary/default.h"

namespace snmalloc
{
  using SecondaryAllocator = DefaultSecondaryAllocator;
} // namespace snmalloc
#endif
