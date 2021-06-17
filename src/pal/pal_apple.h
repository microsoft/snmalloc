#pragma once

#ifdef __APPLE__

#  include "pal_bsd.h"

#  include <CommonCrypto/CommonRandom.h>
#  include <mach/mach_init.h>
#  include <mach/mach_vm.h>
#  include <mach/vm_statistics.h>
#  include <mach/vm_types.h>
#  include <stdio.h>
#  include <stdlib.h>
#  include <sys/mman.h>
#  include <unistd.h>

namespace snmalloc
{
  /**
   * PAL implementation for Apple systems (macOS, iOS, watchOS, tvOS...).
   */
  template<int PALAnonID = PALAnonDefaultID>
  class PALApple : public PALBSD<PALApple<>>
  {
  public:
    /**
     * The features exported by this PAL.
     */
    static constexpr uint64_t pal_features =
      AlignedAllocation | LazyCommit | Entropy;

    static constexpr size_t page_size = Aal::aal_name == ARM ? 0x4000 : 0x1000;

    static constexpr size_t minimum_alloc_size = page_size;

    static constexpr int anonymous_memory_fd = VM_MAKE_TAG(PALAnonID);

    static constexpr int default_mach_vm_map_flags = VM_MAKE_TAG(PALAnonID);

    /**
     * Notify platform that we will not be using these pages.
     */
    static void notify_not_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));

#  ifdef USE_POSIX_COMMIT_CHECKS
      memset(p, 0x5a, size);
#  endif

      // `MADV_FREE_REUSABLE` can only be applied to writable pages,
      // otherwise it's an error.
      //
      // `mach_vm_behavior_set` is observably slower in benchmarks.
      madvise(p, size, MADV_FREE_REUSABLE);

#  ifdef USE_POSIX_COMMIT_CHECKS
      // This must occur after `MADV_FREE_REUSABLE`.
      //
      // `mach_protect` is observably slower in benchmarks.
      mprotect(p, size, PROT_NONE);
#  endif
    }

    /**
     * Notify platform that we will be using these pages.
     */
    template<ZeroMem zero_mem>
    static void notify_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(
        is_aligned_block<page_size>(p, size) || (zero_mem == NoZero));

      if constexpr (zero_mem == YesZero)
      {
        void* r = mmap(
          p,
          size,
          PROT_READ | PROT_WRITE,
          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
          anonymous_memory_fd,
          0);

        if (likely(r != MAP_FAILED))
        {
          return;
        }
      }

#  ifdef USE_POSIX_COMMIT_CHECKS
      // Mark pages as writable for `madvise` below.
      //
      // `mach_vm_protect` is observably slower in benchmarks.
      mprotect(p, size, PROT_READ | PROT_WRITE);
#  endif

      // `MADV_FREE_REUSE` can only be applied to writable pages,
      // otherwise it's an error.
      //
      // `mach_vm_behavior_set` is observably slower in benchmarks.
      madvise(p, size, MADV_FREE_REUSE);

      if constexpr (zero_mem == YesZero)
      {
        ::bzero(p, size);
      }
    }

    template<bool committed>
    static void* reserve_aligned(size_t size) noexcept
    {
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(size >= minimum_alloc_size);

      // mask has least-significant bits set
      mach_vm_offset_t mask = size - 1;

      int flags =
        VM_FLAGS_ANYWHERE | VM_FLAGS_RANDOM_ADDR | default_mach_vm_map_flags;

      // must be initialized to 0 or addr is interepreted as a lower-bound.
      mach_vm_address_t addr = 0;

#  ifdef USE_POSIX_COMMIT_CHECKS
      vm_prot_t prot = committed ? VM_PROT_READ | VM_PROT_WRITE : VM_PROT_NONE;
#  else
      vm_prot_t prot = VM_PROT_READ | VM_PROT_WRITE;
#  endif

      kern_return_t kr = mach_vm_map(
        mach_task_self(),
        &addr,
        size,
        mask,
        flags,
        MEMORY_OBJECT_NULL,
        0,
        TRUE,
        prot,
        VM_PROT_READ | VM_PROT_WRITE,
        VM_INHERIT_COPY);

      if (unlikely(kr != KERN_SUCCESS))
      {
        error("Failed to allocate memory\n");
      }

      return reinterpret_cast<void*>(addr);
    }

    /**
     * Source of Entropy
     *
     * Apple platforms do not have a public getentropy implementation, so use
     * CCRandomGenerateBytes instead.
     */
    static uint64_t get_entropy64()
    {
      uint64_t result;
      if (
        CCRandomGenerateBytes(
          reinterpret_cast<void*>(&result), sizeof(result)) != kCCSuccess)
      {
        error("Failed to get system randomness");
      }

      return result;
    }
  };
} // namespace snmalloc
#endif
