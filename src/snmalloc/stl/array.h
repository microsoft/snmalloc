#pragma once

#include "snmalloc/stl/common.h"

#if SNMALLOC_USE_SELF_VENDORED_STL
#  include "snmalloc/stl/gnu/array.h"
#else
#  include "snmalloc/stl/cxx/array.h"
#endif
