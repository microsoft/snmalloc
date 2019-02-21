#pragma once

#include "pal_plain.h"
#ifdef OPEN_ENCLAVE
extern "C" const void* __oe_get_heap_base();
extern "C" const void* __oe_get_heap_end();
extern "C" void* oe_memset(void* p, int c, size_t size);
extern "C" void oe_abort();

namespace snmalloc
{
  class PALOpenEnclave
  {
    std::atomic<uintptr_t> oe_base;

  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     */
    static constexpr uint64_t pal_features = 0;
    static void error(const char* const str)
    {
      UNUSED(str);
      oe_abort();
    }

    template<bool committed>
    void* reserve(size_t* size, size_t align) noexcept
    {
      if (oe_base == 0)
      {
        uintptr_t dummy = 0;
        oe_base.compare_exchange_strong(dummy, (uintptr_t)__oe_get_heap_base());
      }

      uintptr_t old_base = oe_base;
      uintptr_t old_base2 = old_base;
      uintptr_t next_base;
      auto end = (uintptr_t)__oe_get_heap_end();
      do
      {
        old_base2 = old_base;
        auto new_base = bits::align_up(old_base, align);
        next_base = new_base + *size;

        if (next_base > end)
          error("Out of memory");

      } while (oe_base.compare_exchange_strong(old_base, next_base));

      *size = next_base - old_base2;
      return (void*)old_base;
    }

    template<bool page_aligned = false>
    void zero(void* p, size_t size) noexcept
    {
      oe_memset(p, 0, size);
    }
  };
}
#endif
