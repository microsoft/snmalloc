#define SNMALLOC_TRACING

#define SNMALLOC_NAME_MANGLE(a) host_##a
// Redefine the namespace, so we can have two versions.
#define snmalloc snmalloc_host
#include "../../../override/malloc.cc"
