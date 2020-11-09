#pragma once

#include "../ds/address.h"
#if defined(BACKTRACE_HEADER)
#  include BACKTRACE_HEADER
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

extern "C" int puts(const char* str);

namespace snmalloc
{
  /**
   * Platform abstraction layer for generic POSIX systems.
   *
   * This provides the lowest common denominator for POSIX systems.  It should
   * work on pretty much any POSIX system, but won't necessarily be the most
   * efficient implementation.  Subclasses should provide more efficient
   * implementations using platform-specific functionality.
   *
   * The template parameter for this is the subclass and is used for explicit
   * up casts to allow this class to call non-virtual methods on the templated
   * version.
   */
  template<class OS>
  class PALPOSIX
  {
    /**
     * Helper class to access the `default_mmap_flags` field of `OS` if one
     * exists or a default value if not.  This provides the default version,
     * which is used if `OS::default_mmap_flags` does not exist.
     */
    template<typename T, typename = int>
    struct DefaultMMAPFlags
    {
      /**
       * If `OS::default_mmap_flags` does not exist, use 0.  This value is
       * or'd with the other mmap flags and so a value of 0 is a no-op.
       */
      static const int flags = 0;
    };

    /**
     * Helper class to access the `default_mmap_flags` field of `OS` if one
     * exists or a default value if not.  This provides the version that
     * accesses the field, allowing other PALs to provide extra arguments to
     * the `mmap` calls used here.
     */
    template<typename T>
    struct DefaultMMAPFlags<T, decltype((void)T::default_mmap_flags, 0)>
    {
      static const int flags = T::default_mmap_flags;
    };

    /**
     * Helper class to allow `OS` to provide the file descriptor used for
     * anonymous memory. This is the default version, which provides the POSIX
     * default of -1.
     */
    template<typename T, typename = int>
    struct AnonFD
    {
      /**
       * If `OS::anonymous_memory_fd` does not exist, use -1.  This value is
       * defined by POSIX.
       */
      static const int fd = -1;
    };

    /**
     * Helper class to allow `OS` to provide the file descriptor used for
     * anonymous memory. This exposes the `anonymous_memory_fd` field in `OS`.
     */
    template<typename T>
    struct AnonFD<T, decltype((void)T::anonymous_memory_fd, 0)>
    {
      /**
       * The PAL's provided file descriptor for anonymous memory.  This is
       * used, for example, on Apple platforms, which use the file descriptor
       * in a `MAP_ANONYMOUS` mapping to encode metadata about the owner of the
       * mapping.
       */
      static const int fd = T::anonymous_memory_fd;
    };

  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     *
     * POSIX systems are assumed to support lazy commit.
     */
    static constexpr uint64_t pal_features = LazyCommit;

    static constexpr size_t page_size = 0x1000;

    static void print_stack_trace()
    {
#ifdef BACKTRACE_HEADER
      constexpr int SIZE = 1024;
      void* buffer[SIZE];
      auto nptrs = backtrace(buffer, SIZE);
      fflush(stdout);
      backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO);
      puts("");
      fflush(stdout);
#endif
    }

    /**
     * Report a fatal error an exit.
     */
    [[noreturn]] static void error(const char* const str) noexcept
    {
      puts(str);
      print_stack_trace();
      abort();
    }

    /**
     * Notify platform that we will not be using these pages.
     *
     * This does nothing in a generic POSIX implementation.  Most POSIX systems
     * provide an `madvise` call that can be used to return pages to the OS in
     * high memory pressure conditions, though on Linux this seems to impose
     * too much of a performance penalty.
     */
    static void notify_not_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<OS::page_size>(p, size));
#ifdef USE_POSIX_COMMIT_CHECKS
      // Fill memory so that when we switch the pages back on we don't make
      // assumptions on the content.
      memset(p, 0x5a, size);
      mprotect(p, size, PROT_NONE);
#else
      UNUSED(p);
      UNUSED(size);
#endif
    }

    /**
     * Notify platform that we will be using these pages.
     *
     * On POSIX platforms, lazy commit means that this is a no-op, unless we
     * are also zeroing the pages in which case we call the platform's `zero`
     * function.
     */
    template<ZeroMem zero_mem>
    static void notify_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(
        is_aligned_block<OS::page_size>(p, size) || (zero_mem == NoZero));

#ifdef USE_POSIX_COMMIT_CHECKS
      mprotect(p, size, PROT_READ | PROT_WRITE);
#else
      UNUSED(p);
      UNUSED(size);
#endif

      if constexpr (zero_mem == YesZero)
        zero<true>(p, size);
    }

    /**
     * OS specific function for zeroing memory.
     *
     * The generic POSIX implementation uses mmap to map anonymous memory over
     * the range for ranges larger than a page.  The underlying OS is assumed
     * to provide new CoW copies of the zero page.
     *
     * Note: On most systems it is faster for a single page to zero the memory
     * explicitly than do this, we should probably tweak the threshold for
     * calling bzero at some point.
     */
    template<bool page_aligned = false>
    static void zero(void* p, size_t size) noexcept
    {
      if (page_aligned || is_aligned_block<OS::page_size>(p, size))
      {
        SNMALLOC_ASSERT(is_aligned_block<OS::page_size>(p, size));
        void* r = mmap(
          p,
          size,
          PROT_READ | PROT_WRITE,
          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | DefaultMMAPFlags<OS>::flags,
          AnonFD<OS>::fd,
          0);

        if (r != MAP_FAILED)
          return;
      }

      bzero(p, size);
    }

    /**
     * Reserve memory.
     *
     * POSIX platforms support lazy commit, and so this also puts the memory in
     * the lazy commit state (i.e. pages will be allocated on first use).
     *
     * POSIX does not define a portable interface for specifying alignment
     * greater than a page.
     */
    static std::pair<void*, size_t> reserve_at_least(size_t size) noexcept
    {
      SNMALLOC_ASSERT(size == bits::next_pow2(size));

      // Magic number for over-allocating chosen by the Pal
      // These should be further refined based on experiments.
      constexpr size_t min_size =
        bits::is64() ? bits::one_at_bit(32) : bits::one_at_bit(28);

      for (size_t size_request = bits::max(size, min_size);
           size_request >= size;
           size_request = size_request / 2)
      {
        void* p = mmap(
          nullptr,
          size_request,
          PROT_READ | PROT_WRITE,
          MAP_PRIVATE | MAP_ANONYMOUS | DefaultMMAPFlags<OS>::flags,
          AnonFD<OS>::fd,
          0);

        if (p != MAP_FAILED)
          return {p, size_request};
      }

      OS::error("Out of memory");
    }
  };
} // namespace snmalloc
