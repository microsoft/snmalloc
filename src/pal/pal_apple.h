#pragma once

#ifdef __APPLE__

#  include "../ds/address.h"

#  include <Security/Security.h>
#  include <errno.h>
#  include <execinfo.h>
#  include <mach/mach_init.h>
#  include <mach/mach_vm.h>
#  include <mach/vm_region.h>
#  include <mach/vm_statistics.h>
#  include <mach/vm_types.h>
#  include <utility>

extern "C" int puts(const char* str);

namespace snmalloc
{
  /**
   * PAL implementation for Apple systems (macOS, iOS, watchOS, tvOS...).
   */
  template<int PALAnonID = PALAnonDefaultID>
  class PALApple
  {
  public:
    /**
     * The features exported by this PAL.
     */
    static constexpr uint64_t pal_features =
      AlignedAllocation | LazyCommit | Entropy;

    static constexpr size_t page_size = Aal::aal_name == ARM ? 0x4000 : 0x1000;

    static constexpr size_t minimum_alloc_size = page_size;

    static void print_stack_trace()
    {
      constexpr int SIZE = 1024;
      void* buffer[SIZE];
      auto nptrs = backtrace(buffer, SIZE);
      fflush(stdout);
      backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO);
      puts("");
      fflush(stdout);
    }

    /**
     * Report a fatal error and exit.
     */
    [[noreturn]] static void error(const char* const str) noexcept
    {
      puts(str);
      print_stack_trace();
      abort();
    }

    /**
     * Notify platform that we will not be using these pages.
     */
    static void notify_not_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));

      mach_vm_behavior_set(
        mach_task_self(),
        reinterpret_cast<mach_vm_address_t>(p),
        mach_vm_size_t(size),
        VM_BEHAVIOR_REUSABLE);
    }

    /**
     * Notify platform that we will be using these pages.
     */
    template<ZeroMem zero_mem>
    static void notify_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(
        is_aligned_block<page_size>(p, size) || (zero_mem == NoZero));

      mach_vm_behavior_set(
        mach_task_self(),
        reinterpret_cast<mach_vm_address_t>(p),
        mach_vm_size_t(size),
        VM_BEHAVIOR_REUSE);

      if constexpr (zero_mem == YesZero)
      {
        zero<true>(p, size);
      }
    }

    /**
     * OS specific function for zeroing memory.
     */
    template<bool page_aligned = false>
    static void zero(void* p, size_t size) noexcept
    {
      if (page_aligned || is_aligned_block<page_size>(p, size))
      {
        SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));

        // mask has least-significant bits set
        mach_vm_offset_t mask = page_size - 1;

        int flags =
          VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(PALAnonID);

        mach_vm_address_t addr = reinterpret_cast<mach_vm_address_t>(p);

        kern_return_t kr = mach_vm_map(
          mach_task_self(),
          &addr,
          size,
          mask,
          flags,
          MEMORY_OBJECT_NULL,
          0,
          TRUE,
          VM_PROT_READ | VM_PROT_WRITE,
          VM_PROT_READ | VM_PROT_WRITE,
          VM_INHERIT_COPY);

        if (kr == KERN_SUCCESS)
        {
          return;
        }
      }

      ::bzero(p, size);
    }

    template<bool committed>
    static void* reserve_aligned(size_t size) noexcept
    {
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(size >= minimum_alloc_size);

      // mask has least-significant bits set
      mach_vm_offset_t mask = size - 1;

      int flags =
        VM_FLAGS_ANYWHERE | VM_FLAGS_RANDOM_ADDR | VM_MAKE_TAG(PALAnonID);

      // must be initialized to 0 or addr is interepreted as a lower-bound.
      mach_vm_address_t addr = 0;

      kern_return_t kr = mach_vm_map(
        mach_task_self(),
        &addr,
        size,
        mask,
        flags,
        MEMORY_OBJECT_NULL,
        0,
        TRUE,
        VM_PROT_READ | VM_PROT_WRITE,
        VM_PROT_READ | VM_PROT_WRITE,
        VM_INHERIT_COPY);

      if (kr != KERN_SUCCESS)
      {
        error("Failed to allocate memory\n");
      }

      return reinterpret_cast<void*>(addr);
    }

    /**
     * Source of Entropy
     *
     * Apple platforms do not have a getentropy implementation, so use
     * SecRandomCopyBytes instead.
     */
    static uint64_t get_entropy64()
    {
      uint64_t result;
      if (
        SecRandomCopyBytes(
          kSecRandomDefault,
          sizeof(result),
          reinterpret_cast<void*>(&result)) != errSecSuccess)
      {
        error("Failed to get system randomness");
      }

      return result;
    }
  };
} // namespace snmalloc
#endif