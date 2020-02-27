#pragma once

#include "pal_plain.h"
#ifdef OPEN_ENCLAVE
extern "C" const void* __oe_get_heap_base();
extern "C" const void* __oe_get_heap_end();
extern "C" void* oe_memset_s(void* p, size_t p_size, int c, size_t size);
extern "C" void oe_abort();

namespace snmalloc
{
  class PALOpenEnclave
  {
    std::atomic<void*> oe_base = nullptr;

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
    void* reserve(size_t size) noexcept
    {
      if (oe_base == 0)
      {
        void* dummy = NULL;
        oe_base.compare_exchange_strong(
          dummy, const_cast<void*>(__oe_get_heap_base()));
      }

      void* old_base = oe_base;
      void* next_base;
      auto end = __oe_get_heap_end();
      do
      {
        auto new_base = old_base;
        next_base = pointer_offset(new_base, size);

        if (next_base > end)
          error("Out of memory");

      } while (oe_base.compare_exchange_strong(old_base, next_base));

      return old_base;
    }

    template<bool page_aligned = false>
    void zero(void* p, size_t size) noexcept
    {
      oe_memset_s(p, size, 0, size);
    }
  };
}
#endif
