#undef IS_ADDRESS_SPACE_CONSTRAINED
#define OPEN_ENCLAVE
#define OPEN_ENCLAVE_SIMULATION
#define USE_RESERVE_MULTIPLE 1
#define NO_BOOTSTRAP_ALLOCATOR
#define IS_ADDRESS_SPACE_CONSTRAINED
#define SNMALLOC_EXPOSE_PAGEMAP
#define SNMALLOC_NAME_MANGLE(a) enclave_##a
// Redefine the namespace, so we can have two versions.
#define snmalloc snmalloc_enclave
#include "../../../override/malloc.cc"
