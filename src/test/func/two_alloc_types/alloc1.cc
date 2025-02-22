#ifndef SNMALLOC_TRACING
#  define SNMALLOC_TRACING
#endif

// Redefine the namespace, so we can have two versions.
#define snmalloc snmalloc_enclave

#include <snmalloc/backend/fixedglobalconfig.h>
#include <snmalloc/snmalloc_core.h>

// Specify type of allocator
#define SNMALLOC_PROVIDE_OWN_CONFIG

namespace snmalloc
{
  using Config = FixedRangeConfig<PALNoAlloc<DefaultPal>>;
}

#define SNMALLOC_NAME_MANGLE(a) enclave_##a
#include <snmalloc/override/malloc.cc>

extern "C" void oe_allocator_init(void* base, void* end)
{
  snmalloc::Config::init(nullptr, base, address_cast(end) - address_cast(base));
}
