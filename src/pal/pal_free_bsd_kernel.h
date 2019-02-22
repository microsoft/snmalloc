#pragma once

#include "../ds/bits.h"
#include "../mem/allocconfig.h"

#if defined(FreeBSD_KERNEL)
extern "C"
{
#  include <sys/vmem.h>
#  include <vm/vm.h>
#  include <vm/vm_extern.h>
#  include <vm/vm_kern.h>
#  include <vm/vm_object.h>
#  include <vm/vm_param.h>
}

namespace snmalloc
{
  class PALFreeBSDKernel
  {
    vm_offset_t get_vm_offset(uint_ptr_t p)
    {
      return static_cast<vm_offset_t>(reinterpret_cast<uintptr_t>(p));
    }

  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     */
    static constexpr uint64_t pal_features = AlignedAllocation;
    void error(const char* const str)
    {
      panic("snmalloc error: %s", str);
    }

    /// Notify platform that we will not be using these pages
    void notify_not_using(void* p, size_t size)
    {
      vm_offset_t addr = get_vm_offset(p);
      kmem_unback(kernel_object, addr, size);
    }

    /// Notify platform that we will be using these pages
    template<ZeroMem zero_mem>
    void notify_using(void* p, size_t size)
    {
      vm_offset_t addr = get_vm_offset(p);
      int flags = M_WAITOK | ((zero_mem == YesZero) ? M_ZERO : 0);
      if (kmem_back(kernel_object, addr, size, flags) != KERN_SUCCESS)
      {
        error("Out of memory");
      }
    }

    /// OS specific function for zeroing memory
    template<bool page_aligned = false>
    void zero(void* p, size_t size)
    {
      ::bzero(p, size);
    }

    template<bool committed>
    void* reserve(size_t* size, size_t align)
    {
      size_t request = *size;
      vm_offset_t addr;
      if (vmem_xalloc(
            kernel_arena,
            request,
            align,
            0,
            0,
            VMEM_ADDR_MIN,
            VMEM_ADDR_MAX,
            M_BESTFIT,
            &addr))
      {
        return nullptr;
      }
      if (committed)
      {
        if (
          kmem_back(kernel_object, addr, request, M_ZERO | M_WAITOK) !=
          KERN_SUCCESS)
        {
          vmem_xfree(kernel_arena, addr, request);
          return nullptr;
        }
      }
      return get_vm_offset(addr);
    }
  };
}
#endif
