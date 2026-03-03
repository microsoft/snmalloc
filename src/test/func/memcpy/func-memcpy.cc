// Windows doesn't like changing the linkage spec of abort.
#if defined(_MSC_VER)
int main()
{
  return 0;
}
#else
// QEMU user mode does not support the code that generates backtraces and so we
// also need to skip this test if we are doing a debug build and targeting
// QEMU.
#  if defined(SNMALLOC_QEMU_WORKAROUND) && defined(SNMALLOC_BACKTRACE_HEADER)
#    undef SNMALLOC_BACKTRACE_HEADER
#  endif
#  ifdef SNMALLOC_STATIC_LIBRARY_PREFIX
#    undef SNMALLOC_STATIC_LIBRARY_PREFIX
#  endif
#  ifdef SNMALLOC_FAIL_FAST
#    undef SNMALLOC_FAIL_FAST
#  endif
#  define SNMALLOC_FAIL_FAST false
#  define SNMALLOC_STATIC_LIBRARY_PREFIX my_
#  include "snmalloc/override/malloc.cc"
#  include "snmalloc/override/memcpy.cc"
#  include "test/helpers.h"

#  include <assert.h>
#  include <csetjmp>
#  include <initializer_list>
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>

using namespace snmalloc;

/**
 * Jump buffer used to jump out of `abort()` for recoverable errors.
 */
static std::jmp_buf jmp;

/**
 * Flag indicating whether `jmp` is valid.  If this is set then calls to
 * `abort` will jump to the jump buffer, rather than exiting.
 */
static bool can_longjmp;

/**
 * Replacement for the C standard `abort` that returns to the `setjmp` call for
 * recoverable errors.
 */
extern "C" void abort()
{
  if (can_longjmp)
  {
    longjmp(jmp, 1);
  }
#  if __has_builtin(__builtin_trap)
  __builtin_trap();
#  endif
  exit(-1);
}

/**
 * Check that memcpy works in correct use.  This allocates a pair of buffers,
 * fills one with a well-known pattern, and then copies subsets of this at
 * one-byte increments to a target.  This gives us unaligned starts.
 */
void check_size(size_t size)
{
  START_TEST("checking {}-byte memcpy", size);
  auto* s = static_cast<unsigned char*>(my_malloc(size + 1));
  auto* d = static_cast<unsigned char*>(my_malloc(size + 1));
  d[size] = 0;
  s[size] = 255;
  for (size_t start = 0; start < size; start++)
  {
    unsigned char* src = s + start;
    unsigned char* dst = d + start;
    size_t sz = (size - start);
    for (size_t i = 0; i < sz; ++i)
    {
      src[i] = static_cast<unsigned char>(i);
    }
    for (size_t i = 0; i < sz; ++i)
    {
      dst[i] = 0;
    }
    void* ret = my_memcpy(dst, src, sz);
    EXPECT(ret == dst, "Return value should be {}, was {}", dst, ret);
    for (size_t i = 0; i < sz; ++i)
    {
      if (dst[i] != static_cast<unsigned char>(i))
      {
        fprintf(
          stderr,
          "Testing size %zd %hhx == %hhx\n",
          sz,
          static_cast<unsigned char>(i),
          dst[i]);
      }
      EXPECT(
        dst[i] == (unsigned char)i,
        "dst[i] == {}, i == {}",
        size_t(dst[i]),
        i & 0xff);
    }
    EXPECT(d[size] == 0, "d[size] == {}", d[size]);
  }
  my_free(s);
  my_free(d);
}

void check_bounds(size_t size, size_t out_of_bounds)
{
  START_TEST(
    "memcpy bounds, size {}, {} bytes out of bounds", size, out_of_bounds);
  auto* s = static_cast<unsigned char*>(my_malloc(size));
  auto* d = static_cast<unsigned char*>(my_malloc(size));
  for (size_t i = 0; i < size; ++i)
  {
    s[i] = static_cast<unsigned char>(i);
  }
  for (size_t i = 0; i < size; ++i)
  {
    d[i] = 0;
  }
  bool bounds_error = false;
  can_longjmp = true;
  if (setjmp(jmp) == 0)
  {
    my_memcpy(d, s, size + out_of_bounds);
  }
  else
  {
    bounds_error = true;
  }
  can_longjmp = false;
  EXPECT(
    bounds_error == (out_of_bounds > 0),
    "bounds error: {}, out_of_bounds: {}",
    bounds_error,
    out_of_bounds);
  my_free(s);
  my_free(d);
}

int main()
{
  // Some sizes to check for out-of-bounds access.  As we are only able to
  // catch overflows past the end of the sizeclass-padded allocation, make
  // sure we don't try to test on smaller allocations.

  static constexpr size_t min_class_size =
    sizeclass_to_size(size_to_sizeclass(MIN_ALLOC_SIZE));

  std::initializer_list<size_t> sizes = {min_class_size, 1024, 2 * 1024 * 1024};
  static_assert(
    MIN_ALLOC_SIZE < 1024,
    "Can't detect overflow except at sizeclass boundaries");
  for (auto sz : sizes)
  {
    // Check in bounds
    check_bounds(sz, 0);
    // Check one byte out
    check_bounds(sz, 1);
    // Check one object out of bounds
    check_bounds(sz, sz);
  }
  for (size_t x = 0; x < 2048; x++)
  {
    check_size(x);
  }
}
#endif