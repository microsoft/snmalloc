#ifndef SNMALLOC_TRACING
#  define SNMALLOC_TRACING
#endif

#define SNMALLOC_NAME_MANGLE(a) host_##a
// Redefine the namespace, so we can have two versions.
#define snmalloc snmalloc_host
#include <snmalloc/override/malloc.cc>
