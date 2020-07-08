#undef SNMALLOC_USE_LARGE_CHUNKS
#define OPEN_ENCLAVE
#define OPEN_ENCLAVE_SIMULATION
#define NO_BOOTSTRAP_ALLOCATOR
#define SNMALLOC_EXPOSE_PAGEMAP
#define SNMALLOC_NAME_MANGLE(a) enclave_##a
// Redefine the namespace, so we can have two versions.
#define snmalloc snmalloc_enclave
#include "../../../override/malloc.cc"

extern "C" void oe_allocator_init(void* base, void* end)
{
  snmalloc_enclave::PALOpenEnclave::setup_initial_range(base, end);
}
