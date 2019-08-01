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
  /**
   * OpenBSD platform abstraction layer.
   *
   * OpenBSD behaves exactly like a generic BSD platform but this class exists
   * as a place to add OpenBSD-specific behaviour later, if required.
   */
  class PALOBSD : public PALBSD<PALOBSD>
  {
  public:
    /**
     * The features exported by this PAL.
     *
     * Currently, these are identical to the generic BSD PAL.  This field is
     * declared explicitly to remind anyone who modifies this class that they
     * should add any required features.
     */
    static constexpr uint64_t pal_features = PALBSD::pal_features;
  };
} // namespace snmalloc
#endif
