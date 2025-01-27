#pragma once

#include "gwp_asan/guarded_pool_allocator.h"
#include "snmalloc/ds_core/defines.h"
#include "snmalloc/mem/sizeclasstable.h"
#if defined(SNMALLOC_BACKTRACE_HEADER)
#  include SNMALLOC_BACKTRACE_HEADER
#endif

namespace snmalloc
{
  class GwpAsanSecondaryAllocator
  {
    static size_t max_allocation_size;

    static void
    initialize_gwp_asan(gwp_asan::GuardedPoolAllocator* allocator) noexcept
    {
      // for now, we use default options
      gwp_asan::options::Options opt;
      opt.setDefaults();
#ifdef SNMALLOC_BACKTRACE_HEADER
      opt.Backtrace = [](uintptr_t* buf, size_t length) {
        return static_cast<size_t>(
          ::backtrace(reinterpret_cast<void**>(buf), static_cast<int>(length)));
      };
#endif
      allocator->init(opt);
      max_allocation_size =
        allocator->getAllocatorState()->maximumAllocationSize();
    }

    static gwp_asan::GuardedPoolAllocator& get()
    {
      static Singleton<gwp_asan::GuardedPoolAllocator, initialize_gwp_asan>
        singleton;
      return singleton.get();
    }

  public:
    SNMALLOC_FAST_PATH static void* allocate(size_t size)
    {
      auto& inner = get();
      if (SNMALLOC_UNLIKELY(inner.shouldSample()))
      {
        if (size > max_allocation_size)
          return nullptr;
        auto alignment = natural_alignment(size);
        return get().allocate(size, alignment);
      }
      return nullptr;
    }

    SNMALLOC_FAST_PATH
    static void deallocate(void* pointer)
    {
      if (SNMALLOC_LIKELY(pointer == nullptr))
        return;

      auto& inner = get();
      snmalloc_check_client(
        mitigations(sanity_checks),
        inner.pointerIsMine(pointer),
        "Not allocated by snmalloc or secondary allocator");

      inner.deallocate(pointer);
    }

    SNMALLOC_FAST_PATH
    static bool has_secondary_ownership([[maybe_unused]] void* pointer)
    {
      return get().pointerIsMine(pointer);
    }
  };

  inline size_t GwpAsanSecondaryAllocator::max_allocation_size;
} // namespace snmalloc
