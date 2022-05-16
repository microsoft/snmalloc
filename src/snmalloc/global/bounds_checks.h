#pragma once
#include "threadalloc.h"

namespace snmalloc
{
  /**
   * Should we check loads?  This defaults to on in debug builds, off in
   * release (store-only checks) and can be overridden by defining the macro
   * `SNMALLOC_CHECK_LOADS` to true or false.
   */
  static constexpr bool CheckReads =
#ifdef SNMALLOC_CHECK_LOADS
    SNMALLOC_CHECK_LOADS
#else
    DEBUG
#endif
    ;

  /**
   * Should we fail fast when we encounter an error?  With this set to true, we
   * just issue a trap instruction and crash the process once we detect an
   * error. With it set to false we print a helpful error message and then crash
   * the process.  The process may be in an undefined state by the time the
   * check fails, so there are potentially security implications to turning this
   * off. It defaults to false for both debug and release builds and
   * can be overridden by defining the macro `SNMALLOC_FAIL_FAST` to true or
   * false.
   *
   * Current default to true will help with adoption experience.
   */
  static constexpr bool FailFast =
#ifdef SNMALLOC_FAIL_FAST
    SNMALLOC_FAIL_FAST
#else
    false
#endif
    ;

  /**
   * Report an error message for a failed bounds check and then abort the
   * program.
   * `p` is the input pointer and `len` is the offset from this pointer of the
   * bounds.  `msg` is the message that will be reported along with the
   * start and end of the real object's bounds.
   *
   * Note that this function never returns.  We do not mark it [[NoReturn]]
   * so as to generate better code. The function claims to return a void*,
   * this is so it can be tail called in memcpy.  Note [[NoReturn]] prevents
   * tailcails in GCC and Clang.
   */
  SNMALLOC_SLOW_PATH SNMALLOC_UNUSED_FUNCTION inline void*
  report_fatal_bounds_error(const void* ptr, size_t len, const char* msg)
  {
    if constexpr (FailFast)
    {
      UNUSED(ptr, len, msg);
      SNMALLOC_FAST_FAIL();
    }
    else
    {
      auto& alloc = ThreadAlloc::get();
      void* p = const_cast<void*>(ptr);

      auto copy_end = pointer_offset(p, len);
      auto object_end = alloc.template external_pointer<OnePastEnd>(p);
      report_fatal_error(
        "Fatal Error!\n{}: \n\tcopy range [{}, {})\n\tallocation [{}, "
        "{})\nrange goes beyond allocation by {} bytes \n",
        msg,
        p,
        copy_end,
        alloc.template external_pointer<Start>(p),
        object_end,
        pointer_diff(object_end, copy_end));
    }
  }

  /**
   * Check whether a pointer + length is in the same object as the pointer.
   *
   * Returns true if the checks succeeds.
   */
  SNMALLOC_FAST_PATH_INLINE bool check_bounds(const void* ptr, size_t len)
  {
    auto& alloc = ThreadAlloc::get();

    return alloc.check_bounds(ptr, len);
  }
} // namespace snmalloc
