#pragma once

#include "snmalloc/stl/common.h"

#if SNMALLOC_USE_SELF_VENDORED_STL
#  include "snmalloc/stl/gnu/type_traits.h"
#else
#  include "snmalloc/stl/cxx/type_traits.h"
#endif
