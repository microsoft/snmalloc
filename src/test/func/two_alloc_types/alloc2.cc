#undef IS_ADDRESS_SPACE_CONSTRAINED
#define SNMALLOC_NAME_MANGLE(a) host_##a
#define NO_BOOTSTRAP_ALLOCATOR
#ifndef SNMALLOC_EXPOSE_PAGEMAP
#  define SNMALLOC_EXPOSE_PAGEMAP
#endif
// Redefine the namespace, so we can have two versions.
#define snmalloc snmalloc_host
#include "../../../override/malloc.cc"
