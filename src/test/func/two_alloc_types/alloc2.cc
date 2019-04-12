#undef IS_ADDRESS_SPACE_CONSTRAINED
#define SNMALLOC_NAME_MANGLE(a) host_##a
#define NO_BOOTSTRAP_ALLOCATOR
#define SNMALLOC_EXPOSE_PAGEMAP
// Redefine the namespace, so we can have two versions.
#define snmalloc snmalloc_host
#include "../../../override/malloc.cc"
