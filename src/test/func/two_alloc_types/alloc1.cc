#define SNMALLOC_TRACING

// Redefine the namespace, so we can have two versions.
#define snmalloc snmalloc_enclave

#include <mem/fixedglobalconfig.h>
#include <snmalloc_core.h>

// Specify type of allocator
#define SNMALLOC_PROVIDE_OWN_CONFIG
namespace snmalloc
{
  using Alloc = FastAllocator<FixedGlobals>;
}

#define SNMALLOC_NAME_MANGLE(a) enclave_##a
#include "../../../override/malloc.cc"

extern "C" void oe_allocator_init(void* base, void* end)
{
  snmalloc::FixedGlobals fixed_handle;
  fixed_handle.init(
    CapPtr<void, CBChunk>(base), address_cast(end) - address_cast(base));
}
