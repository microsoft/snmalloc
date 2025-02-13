#pragma once

// Core implementation of snmalloc independent of the configuration mode
#include "snmalloc_core.h"

// Provides the global configuration for the snmalloc implementation.
#include "backend/globalconfig.h"

namespace snmalloc
{
// If you define SNMALLOC_PROVIDE_OWN_CONFIG then you must provide your own
// definition of `snmalloc::Alloc` before including any files that include
// `snmalloc.h` or consume the global allocation APIs.
#ifndef SNMALLOC_PROVIDE_OWN_CONFIG
  using Config = snmalloc::StandardConfigClientMeta<NoClientMetaDataProvider>;
#endif
  /**
   * Create allocator type for this configuration.
   */
  using Alloc = snmalloc::LocalAllocator<Config>;
} // namespace snmalloc

// User facing API surface, needs to know what `Alloc` is.
#include "snmalloc_front.h"
