#include "detours.h"

#pragma warning(push)
#pragma warning(disable : 4075)
// This pragma uses a segment that is alphabetically later than the
// one used in pal_windows.h.  This is required to ensure that the
// global function pointers have been initialized before we attempt to
// detour them.
#pragma init_seg(".CRT$XCV")
static SnmallocDetour snmalloc_detour;
#pragma warning(pop)