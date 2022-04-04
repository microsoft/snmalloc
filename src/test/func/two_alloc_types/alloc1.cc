//#define SNMALLOC_TRACING

// Redefine the namespace, so we can have two versions.
#define snmalloc snmalloc_enclave

#include <backend/fixedglobalconfig.h>
#include <snmalloc_core.h>

// Specify type of allocator
#define SNMALLOC_PROVIDE_OWN_CONFIG
namespace snmalloc
{
  using CustomGlobals = FixedGlobals<PALNoAlloc<DefaultPal>>;
  using Alloc = LocalAllocator<CustomGlobals>;
}

#define SNMALLOC_NAME_MANGLE(a) enclave_##a
#include "../../../override/malloc.cc"

extern "C" void oe_allocator_init(void* base, void* end)
{
  snmalloc::CustomGlobals::init(
    nullptr, base, address_cast(end) - address_cast(base));
}
