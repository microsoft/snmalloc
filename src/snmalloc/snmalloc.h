#pragma once

// Core implementation of snmalloc independent of the configuration mode
#include "snmalloc_core.h"

// If the user has defined SNMALLOC_PROVIDE_OWN_CONFIG, this include does
// nothing.  Otherwise, it provide a default configuration of snmalloc::Alloc.
#include "backend/globalconfig.h"
// User facing API surface, needs to know what `Alloc` is.
#include "snmalloc_front.h"
