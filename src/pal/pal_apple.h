#pragma once

#ifdef __APPLE__
#  include "pal_bsd.h"

namespace snmalloc
{
  /**
   * PAL implementation for Apple systems (macOS, iOS, watchOS, tvOS...).
   *
   * XNU behaves exactly like a generic BSD platform but this class exists
   * as a place to add XNU-specific behaviour later, if required.
   */
  class PALApple : public PALBSD<PALApple>
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
