#pragma once

#ifdef SNMALLOC_TRACING
#  include <iostream>
#endif
#include "../ds/address.h"
#if defined(SNMALLOC_BACKTRACE_HEADER)
#  include SNMALLOC_BACKTRACE_HEADER
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#if __has_include(<sys/random.h>)
#  include <sys/random.h>
#endif
#if __has_include(<unistd.h>)
#  include <unistd.h>
#endif

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

  protected:
    /**
     * A RAII class to capture and restore errno
     */
    class KeepErrno
    {
      int cached_errno;

    public:
      KeepErrno() : cached_errno(errno) {}

      ~KeepErrno()
      {
        errno = cached_errno;
      }
    };

  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     *
     * POSIX systems are assumed to support lazy commit. The build system checks
     * getentropy is available, only then this PAL supports Entropy.
     */
    static constexpr uint64_t pal_features = LazyCommit
#if defined(SNMALLOC_PLATFORM_HAS_GETENTROPY)
      | Entropy
#endif
      ;

    static constexpr size_t page_size = Aal::smallest_page_size;

    /**
     * Address bits are potentially mediated by some POSIX OSes, but generally
     * default to the architecture's.
     *
     * Unlike the AALs, which are composited by explicitly delegating to their
     * template parameters and so play a SFINAE-based game to achieve similar
     * ends, for the PALPOSIX<> classes we instead use more traditional
     * inheritance (e.g., PALLinux is subtype of PALPOSIX<PALLinux>) and so we
     * can just use that mechanism here, too.
     */
    static constexpr size_t address_bits = Aal::address_bits;

    static void print_stack_trace()
    {
#ifdef SNMALLOC_BACKTRACE_HEADER
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
#ifdef SNMALLOC_CHECK_CLIENT
      // Fill memory so that when we switch the pages back on we don't make
      // assumptions on the content.
#  if !defined(NDEBUG)
      memset(p, 0x5a, size);
#  endif
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
     * function, or we have initially mapped the pages as PROT_NONE.
     */
    template<ZeroMem zero_mem>
    static void notify_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(
        is_aligned_block<OS::page_size>(p, size) || (zero_mem == NoZero));

#ifdef SNMALLOC_CHECK_CLIENT
      mprotect(p, size, PROT_READ | PROT_WRITE);
#else
      UNUSED(p);
      UNUSED(size);
#endif

      if constexpr (zero_mem == YesZero)
        zero<true>(p, size);
    }

    /**
     * Notify platform that we will be using these pages for reading.
     *
     * On POSIX platforms, lazy commit means that this is a no-op, unless
     * we have initially mapped the pages as PROT_NONE.
     */
    static void notify_using_readonly(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<OS::page_size>(p, size));

#ifdef SNMALLOC_CHECK_CLIENT
      mprotect(p, size, PROT_READ);
#else
      UNUSED(p);
      UNUSED(size);
#endif
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

        /*
         * If mmap fails, we're going to fall back to zeroing the memory
         * ourselves, which is not stellar, but correct.  However, mmap() will
         * have has left errno nonzero in an effort to explain its MAP_FAILED
         * result.  Capture its current value and restore it at the end of this
         * block.
         */
        auto hold = KeepErrno();

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
    static void* reserve(size_t size) noexcept
    {
#ifdef SNMALLOC_CHECK_CLIENT
      auto prot = PROT_NONE;
#else
      auto prot = PROT_READ | PROT_WRITE;
#endif

      void* p = mmap(
        nullptr,
        size,
        prot,
        MAP_PRIVATE | MAP_ANONYMOUS | DefaultMMAPFlags<OS>::flags,
        AnonFD<OS>::fd,
        0);

      if (p != MAP_FAILED)
      {
#ifdef SNMALLOC_TRACING
        std::cout << "Pal_posix reserved: " << p << " (" << size << ")"
                  << std::endl;
#endif
        return p;
      }

      return nullptr;
    }

    /**
     * Source of Entropy
     *
     * This is a default that works on many POSIX platforms.
     */
    static uint64_t get_entropy64()
    {
      if constexpr (!pal_supports<Entropy, OS>)
      {
        // Derived Pal does not provide entropy.
        return 0;
      }
      else if constexpr (OS::get_entropy64 != get_entropy64)
      {
        // Derived Pal has provided a custom definition.
        return OS::get_entropy64();
      }
      else
      {
#ifdef SNMALLOC_PLATFORM_HAS_GETENTROPY
        uint64_t result;
        if (getentropy(&result, sizeof(result)) != 0)
          error("Failed to get system randomness");
        return result;
#endif
      }
      error("Entropy requested on platform that does not provide entropy");
    }
  };
} // namespace snmalloc
