#pragma once

// Core implementation of snmalloc independent of the configuration mode
#include "../snmalloc_core.h"

#ifndef SNMALLOC_PROVIDE_OWN_CONFIG
#  include "../backend/globalconfig.h"
// The default configuration for snmalloc is used if alternative not defined
namespace snmalloc
{
  using Alloc = snmalloc::LocalAllocator<snmalloc::Globals>;
} // namespace snmalloc
#endif

// User facing API surface, needs to know what `Alloc` is.
#include "../snmalloc_front.h"

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif
#ifdef SNMALLOC_STATIC_LIBRARY_PREFIX
#  define __SN_CONCAT(a, b) a##b
#  define __SN_EVALUATE(a, b) __SN_CONCAT(a, b)
#  define SNMALLOC_NAME_MANGLE(a) \
    __SN_EVALUATE(SNMALLOC_STATIC_LIBRARY_PREFIX, a)
#elif !defined(SNMALLOC_NAME_MANGLE)
#  define SNMALLOC_NAME_MANGLE(a) a
#endif
