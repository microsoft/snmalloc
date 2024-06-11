#pragma once

// Core implementation of snmalloc independent of the configuration mode
#include "snmalloc_core.h"

// Provides the global configuration for the snmalloc implementation.
#include "backend/globalconfig.h"

// If you define SNMALLOC_PROVIDE_OWN_CONFIG then you must provide your own
// definition of `snmalloc::Alloc` before including any files that include
// `snmalloc.h` or consume the global allocation APIs.
#ifndef SNMALLOC_PROVIDE_OWN_CONFIG
namespace snmalloc
{
  /**
   * Create allocator type for this configuration.
   */
  using Alloc = snmalloc::LocalAllocator<
    snmalloc::StandardConfigClientMeta<NoClientMetaDataProvider>>;
} // namespace snmalloc
#endif

// User facing API surface, needs to know what `Alloc` is.
#include "snmalloc_front.h"
