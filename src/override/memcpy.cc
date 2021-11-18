#include "override.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#if __has_include(<xlocale.h>)
#  include <xlocale.h>
#endif

using namespace snmalloc;

// glibc lacks snprintf_l
#ifdef __linux__
#  define snprintf_l(buf, size, loc, msg, ...) \
    snprintf(buf, size, msg, __VA_ARGS__)
// Windows has it with an underscore prefix
#elif defined(_MSC_VER)
#  define snprintf_l(buf, size, loc, msg, ...) \
    _snprintf_s_l(buf, size, _TRUNCATE, msg, loc, __VA_ARGS__)
#endif

namespace
{
  /**
   * Should we check loads?  This defaults to on in debug builds, off in
   * release (store-only checks)
   */
  static constexpr bool CheckReads =
#ifdef SNMALLOC_CHECK_LOADS
    SNMALLOC_CHECK_LOADS
#else
#  ifdef NDEBUG
    false
#  else
    true
#  endif
#endif
    ;

  /**
   * Should we fail fast when we encounter an error?  With this set to true, we
   * just issue a trap instruction and crash the process once we detect an
   * error. With it set to false we print a helpful error message and then crash
   * the process.  The process may be in an undefined state by the time the
   * check fails, so there are potentially security implications to turning this
   * off. It defaults to true for debug builds, false for release builds.
   */
  static constexpr bool FailFast =
#ifdef SNMALLOC_FAIL_FAST
    SNMALLOC_FAIL_FAST
#else
#  ifdef NDEBUG
    true
#  else
    false
#  endif
#endif
    ;

  /**
   * The largest register size that we can use for loads and stores.  These
   * types are expected to work for overlapping copies: we can always load them
   * into a register and store them.  Note that this is at the C abstract
   * machine level: the compiler may spill temporaries to the stack, just not
   * to the source or destination object.
   */
  static constexpr size_t LargestRegisterSize =
#ifdef __AVX__
    32
#elif defined(__SSE__)
    16
#else
    sizeof(uint64_t)
#endif
    ;

  /**
   * Copy a single element of a specified size.  Uses a compiler builtin that
   * expands to a single load and store.
   */
  template<size_t Size>
  SNMALLOC_FAST_PATH_INLINE void copy_one(void* dst, const void* src)
  {
#if __has_builtin(__builtin_memcpy_inline)
    __builtin_memcpy_inline(dst, src, Size);
#else
    // Define a structure of size `Size` that has alignment 1 and a default
    // copy-assignment operator.  We can then copy the data as this type.  The
    // compiler knows the exact width and so will generate the correct wide
    // instruction for us (clang 10 and gcc 12 both generate movups for the
    // 16-byte version of this when targeting SSE.
    struct Block
    {
      char data[Size];
    };
    auto* d = static_cast<Block*>(dst);
    auto* s = static_cast<const Block*>(src);
    *d = *s;
#endif
  }

  SNMALLOC_SLOW_PATH SNMALLOC_UNUSED_FUNCTION void crashWithMessage
    [[noreturn]] (
      void* p, size_t len, const char* msg, decltype(ThreadAlloc::get())& alloc)
  {
    // We're going to crash the program now, but try to avoid heap
    // allocations if possible, since the heap may be in an undefined
    // state.
    std::array<char, 1024> buffer;
    snprintf_l(
      buffer.data(),
      buffer.size(),
      /* Force C locale */ nullptr,
      "%s: %p is in allocation %p--%p, offset 0x%zx is past the end.\n",
      msg,
      p,
      alloc.template external_pointer<Start>(p),
      alloc.template external_pointer<OnePastEnd>(p),
      len);
    Pal::error(buffer.data());
  }

  /**
   * Check whether a pointer + length is in the same object as the pointer.
   * Fail with the error message from the third argument if not.
   *
   * The template parameter indicates whether this is a read.  If so, this
   * function is a no-op when `CheckReads` is false.
   */
  template<bool IsRead = false>
  SNMALLOC_FAST_PATH_INLINE void
  check_bounds(const void* ptr, size_t len, const char* msg = "")
  {
    if constexpr (!IsRead || CheckReads)
    {
      auto& alloc = ThreadAlloc::get();
      void* p = const_cast<void*>(ptr);

      if (SNMALLOC_UNLIKELY(alloc.remaining_bytes(ptr) < len))
      {
        if constexpr (FailFast)
        {
          UNUSED(ptr, len, msg);
          SNMALLOC_FAST_FAIL();
        }
        else
        {
          crashWithMessage(p, len, msg, alloc);
        }
      }
    }
    else
    {
      UNUSED(ptr, len, msg);
    }
  }

  /**
   * Copy a block using the specified size.  This copies as many complete
   * chunks of size `Size` as are possible from `len`.
   */
  template<size_t Size>
  SNMALLOC_FAST_PATH_INLINE void
  block_copy(void* dst, const void* src, size_t len)
  {
    for (size_t i = 0; (i + Size) <= len; i += Size)
    {
      copy_one<Size>(pointer_offset(dst, i), pointer_offset(src, i));
    }
  }

  /**
   * Perform an overlapping copy of the end.  This will copy one (potentially
   * unaligned) `T` from the end of the source to the end of the destination.
   * This may overlap other bits of the copy.
   */
  template<size_t Size>
  SNMALLOC_FAST_PATH_INLINE void
  copy_end(void* dst, const void* src, size_t len)
  {
    copy_one<Size>(
      pointer_offset(dst, len - Size), pointer_offset(src, len - Size));
  }

  /**
   * Predicate indicating whether the source and destination are sufficiently
   * aligned to be copied as aligned chunks of `Size` bytes.
   */
  template<size_t Size>
  SNMALLOC_FAST_PATH bool is_aligned_memcpy(void* dst, const void* src)
  {
    return (pointer_align_down<Size>(const_cast<void*>(src)) == src) &&
      (pointer_align_down<Size>(dst) == dst);
  }
}

extern "C"
{
  /**
   * Snmalloc checked memcpy.
   */
  SNMALLOC_EXPORT void*
    SNMALLOC_NAME_MANGLE(memcpy)(void* dst, const void* src, size_t len)
  {
    // 0 is a very common size for memcpy and we don't need to do external
    // pointer checks if we hit it.  It's also the fastest case, to encourage
    // the compiler to favour the other cases.
    if (SNMALLOC_UNLIKELY(len == 0))
    {
      return dst;
    }
    // Check the bounds of the arguments.
    check_bounds(
      dst, len, "memcpy with destination out of bounds of heap allocation");
    check_bounds<true>(
      src, len, "memcpy with source out of bounds of heap allocation");
    // If this is a small size, do byte-by-byte copies.
    if (len < LargestRegisterSize)
    {
      block_copy<1>(dst, src, len);
      return dst;
    }
    block_copy<LargestRegisterSize>(dst, src, len);
    copy_end<LargestRegisterSize>(dst, src, len);
    return dst;
  }
}
