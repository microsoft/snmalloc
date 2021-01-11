#pragma once

namespace snmalloc
{
  /**
   * Platform abstraction layer that does not allow allocation.
   *
   * This is a minimal PAL for pre-reserved memory regions, where the
   * address-space manager is initialised with all of the memory that it will
   * ever use.
   *
   * It takes an error handler delegate as a template argument. This is
   * expected to forward to the default PAL in most cases.
   */
  template<typename ErrorHandler>
  struct PALNoAlloc
  {
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     */
    static constexpr uint64_t pal_features = NoAllocation;

    static constexpr size_t page_size = Aal::smallest_page_size;

    /**
     * Print a stack trace.
     */
    static void print_stack_trace()
    {
      ErrorHandler::print_stack_trace();
    }

    /**
     * Report a fatal error an exit.
     */
    [[noreturn]] static void error(const char* const str) noexcept
    {
      ErrorHandler::error(str);
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
      memset(p, 0, size);
    }
  };
} // namespace snmalloc
