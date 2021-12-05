#pragma once

// Core implementation of snmalloc independent of the configuration mode
#include "snmalloc_core.h"

// If you define SNMALLOC_PROVIDE_OWN_CONFIG then you must provide your own
// definition of `snmalloc::Alloc` and include `snmalloc_front.h` before
// including any files that include `snmalloc.h` and consume the global
// allocation APIs.
#ifndef SNMALLOC_PROVIDE_OWN_CONFIG
// Default implementation of global state
#  include "backend/globalconfig.h"

// The default configuration for snmalloc
namespace snmalloc
{
  template<SNMALLOC_CONCEPT(snmalloc::ConceptBackendGlobals) GC = snmalloc::Globals> using Alloc = snmalloc::LocalAllocator<GC>;
}

// User facing API surface, needs to know what `Alloc` is.
#  include "snmalloc_front.h"
#endif
