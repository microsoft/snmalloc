#pragma once

#include "../mem/mem.h"
#include "authmap.h"
#include "buddy.h"
#include "commitrange.h"
#include "commonconfig.h"
#include "defaultpagemapentry.h"
#include "empty_range.h"
#include "fragstats.h"
#include "globalrange.h"
#include "indirectrange.h"
#include "largebuddyrange.h"
#include "logrange.h"
#include "noprange.h"
#include "pagemap.h"
#include "pagemapregisterrange.h"
#include "palrange.h"
#include "range_helpers.h"
#include "smallbuddyrange.h"
#include "staticconditionalrange.h"
#include "statsrange.h"
#include "subrange.h"

#ifdef SNMALLOC_PROFILE
// Pull in the H1/A1 hook bodies once commonconfig.h's
// LazyArrayClientMetaDataProvider is visible.  Forward-declared in
// mem/corealloc.h; defined here so any TU that goes through
// snmalloc_core.h sees the full template definition at instantiation
// time.
#  include "../profile/record.h"
#endif
