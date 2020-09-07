#pragma once

#include "ds/address.h"
#include "ds/flaglock.h"
#include "pal_plain.h"

#include <array>
#ifdef OPEN_ENCLAVE
extern "C" void* oe_memset_s(void* p, size_t p_size, int c, size_t size);
extern "C" [[noreturn]] void oe_abort();

namespace snmalloc
{
  class PALOpenEnclave
  {
    /// Base of OE heap
    static inline void* heap_base = nullptr;

    /// Size of OE heap
    static inline size_t heap_size;

    // This is infrequently used code, a spin lock simplifies the code
    // considerably, and should never be on the fast path.
    static inline std::atomic_flag spin_lock;

  public:
    /**
     * This will be called by oe_allocator_init to set up enclave heap bounds.
     */
    static void setup_initial_range(void* base, void* end)
    {
      heap_size = pointer_diff(base, end);
      heap_base = base;
    }

    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     */
    static constexpr uint64_t pal_features = 0;

    static constexpr size_t page_size = 0x1000;

    [[noreturn]] static void error(const char* const str)
    {
      UNUSED(str);
      oe_abort();
    }

    static std::pair<void*, size_t>
    reserve_at_least(size_t request_size) noexcept
    {
      // First call returns the entire address space
      // subsequent calls return {nullptr, 0}
      FlagLock lock(spin_lock);
      if (request_size > heap_size)
        return {nullptr, 0};

      auto result = std::make_pair(heap_base, heap_size);
      heap_size = 0;
      return result;
    }

    template<bool page_aligned = false>
    static void zero(void* p, size_t size) noexcept
    {
      oe_memset_s(p, size, 0, size);
    }
  };
}
#endif
