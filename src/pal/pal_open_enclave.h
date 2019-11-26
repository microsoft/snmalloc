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
    std::atomic<void*> oe_base;

  public:
    static constexpr size_t ADDRESS_BITS = bits::is64() ? 48 : 32;

    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     */
    static constexpr uint64_t pal_features = AlignedAllocation;
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
        void* dummy = NULL;
        oe_base.compare_exchange_strong(
          dummy, const_cast<void*>(__oe_get_heap_base()));
      }

      void* old_base = oe_base;
      void* old_base2 = old_base;
      void* next_base;
      auto end = __oe_get_heap_end();
      do
      {
        old_base2 = old_base;
        auto new_base = pointer_align_up(old_base, align);
        next_base = pointer_offset(new_base, *size);

        if (next_base > end)
          error("Out of memory");

      } while (oe_base.compare_exchange_strong(old_base, next_base));

      *size = pointer_diff(old_base2, next_base);
      return old_base;
    }

    template<bool page_aligned = false>
    void zero(void* p, size_t size) noexcept
    {
      oe_memset(p, 0, size);
    }
  };
}
#endif
