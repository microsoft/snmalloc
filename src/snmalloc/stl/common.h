#pragma once

#include "snmalloc/ds_core/defines.h"

// Default on using the system STL.
#ifndef SNMALLOC_USE_SELF_VENDORED_STL
#  define SNMALLOC_USE_SELF_VENDORED_STL 0
#endif

// Check that the vendored STL is only used with GNU/Clang extensions.
#if SNMALLOC_USE_SELF_VENDORED_STL
#  if !defined(__GNUC__) && !defined(__clang__)
#    error "cannot use vendored STL without GNU/Clang extensions"
#  endif
#endif
