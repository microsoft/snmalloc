#pragma once

#if defined(__OpenBSD__) && !defined(_KERNEL)
#  include "../ds/bits.h"
#  include "../mem/allocconfig.h"
#  include "pal_bsd.h"

#  include <stdio.h>
#  include <string.h>
#  include <sys/mman.h>

namespace snmalloc
{
  class PALOBSD : public PALBSD
  {
  public:
    static constexpr uint64_t pal_features = LazyCommit;
  };
} // namespace snmalloc
#endif
