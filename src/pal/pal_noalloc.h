#ifndef SNMALLOC_PAL_NO_ALLOC_H
#define SNMALLOC_PAL_NO_ALLOC_H

#pragma once

#include "../aal/aal.h"
#include "pal_consts.h"

#include <cstring>

namespace snmalloc
{
  /**
   * Platform abstraction layer that does not allow allocation.
   *
   * This is a minimal PAL for pre-reserved memory regions, where the
   * address-space manager is initialised with all of the memory that it will
   * ever use.
   *
   * It takes an underlying PAL delegate as a template argument. This is
   * expected to forward to the default PAL in most cases.
   */
  template<SNMALLOC_CONCEPT(ConceptPAL) UnderlyingPAL>
  struct PALNoAlloc
  {
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     */
    static constexpr uint64_t pal_features = NoAllocation |
      ((UnderlyingPAL::pal_features & Entropy) != 0 ? Entropy : 0);

    static constexpr size_t page_size = Aal::smallest_page_size;

    /**
     * Print a stack trace.
     */
    static void print_stack_trace()
    {
      UnderlyingPAL::print_stack_trace();
    }

    /**
     * Report a fatal error an exit.
     */
    [[noreturn]] static void error(const char* const str) noexcept
    {
      UnderlyingPAL::error(str);
    }

    /**
     * Notify platform that we will not be using these pages.
     *
     * This is a no-op in this stub.
     */
    static void notify_not_using(void*, size_t) noexcept {}

    /**
     * Notify platform that we will be using these pages.
     *
     * This is a no-op in this stub, except for zeroing memory if required.
     */
    template<ZeroMem zero_mem>
    static void notify_using(void* p, size_t size) noexcept
    {
      if constexpr (zero_mem == YesZero)
      {
        zero<true>(p, size);
      }
      else
      {
        UNUSED(p);
        UNUSED(size);
      }
    }

    /**
     * OS specific function for zeroing memory.
     *
     * This just calls memset - we don't assume that we have access to any
     * virtual-memory functions.
     */
    template<bool page_aligned = false>
    static void zero(void* p, size_t size) noexcept
    {
      UnderlyingPAL::zero(p, size);
    }

    static std::enable_if_t<(pal_features & Entropy) != 0, uint64_t>
    get_entropy64()
    {
      return UnderlyingPAL::get_entropy64();
    }
  };
} // namespace snmalloc

#endif
