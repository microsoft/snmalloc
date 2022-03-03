#pragma once
#include "../snmalloc.h"

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
   * off. It defaults to true for debug builds, false for release builds and
   * can be overridden by defining the macro `SNMALLOC_FAIL_FAST` to true or
   * false.
   */
  static constexpr bool FailFast =
#ifdef SNMALLOC_FAIL_FAST
    SNMALLOC_FAIL_FAST
#else
    !DEBUG
#endif
    ;

  /**
   * Report an error message for a failed bounds check and then abort the
   * program.
   * `p` is the input pointer and `len` is the offset from this pointer of the
   * bounds.  `msg` is the message that will be reported along with the
   * start and end of the real object's bounds.
   */
  SNMALLOC_SLOW_PATH SNMALLOC_UNUSED_FUNCTION inline void
    report_fatal_bounds_error [[noreturn]] (
      void* p, size_t len, const char* msg, decltype(ThreadAlloc::get())& alloc)
  {
    report_fatal_error(
      "{}: {} is in allocation {}--{}, offset {} is past the end\n",
      msg,
      p,
      alloc.template external_pointer<Start>(p),
      alloc.template external_pointer<OnePastEnd>(p),
      len);
  }

  /**
   * The direction for a bounds check.
   */
  enum class CheckDirection
  {
    /**
     * A read bounds check, performed only when read checks are enabled.
     */
    Read,

    /**
     * A write bounds check, performed unconditionally.
     */
    Write
  };

  /**
   * Check whether a pointer + length is in the same object as the pointer.
   * Fail with the error message from the third argument if not.
   *
   * The template parameter indicates whether this is a read.  If so, this
   * function is a no-op when `CheckReads` is false.
   */
  template<CheckDirection Direction = CheckDirection::Write, bool CheckBoth = CheckReads>
  SNMALLOC_FAST_PATH_INLINE void
  check_bounds(const void* ptr, size_t len, const char* msg = "")
  {
    if constexpr ((Direction == CheckDirection::Write) || CheckBoth)
    {
      auto& alloc = ThreadAlloc::get();
      void* p = const_cast<void*>(ptr);

      if (SNMALLOC_UNLIKELY(!alloc.check_bounds(ptr, len)))
      {
        if constexpr (FailFast)
        {
          UNUSED(p, len, msg);
          SNMALLOC_FAST_FAIL();
        }
        else
        {
          report_fatal_bounds_error(p, len, msg, alloc);
        }
      }
    }
    else
    {
      UNUSED(ptr, len, msg);
    }
  }

}
