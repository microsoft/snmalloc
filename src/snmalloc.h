#pragma once

// Core implementation of snmalloc independent of the configuration mode
#include "snmalloc_core.h"

// The default configuration for snmalloc
namespace snmalloc
{
  using Alloc = snmalloc::FastAllocator<snmalloc::Globals>;
}

// User facing API surface, needs to know what `Alloc` is.
#include "snmalloc_front.h"