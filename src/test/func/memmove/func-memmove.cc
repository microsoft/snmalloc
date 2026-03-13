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
 * Check that memmove works correctly for overlapping regions.
 * Allocates a buffer, fills it with a pattern, and tests memmove
 * with various overlap scenarios.
 */
void check_overlaps(size_t size)
{
  START_TEST("checking {}-byte memmove overlaps", size);

  // We need a buffer large enough for the overlap tests.
  // Use 2*size so we can slide src/dst within it.
  size_t bufsize = 2 * size + 1;
  auto* buf = static_cast<unsigned char*>(my_malloc(bufsize));
  auto* ref = static_cast<unsigned char*>(my_malloc(bufsize));

  for (size_t overlap_amount :
       {size_t(1), size / 4 + 1, size / 2 + 1, size - (size > 1 ? 1 : 0), size})
  {
    if (overlap_amount > size || overlap_amount == 0)
      continue;

    size_t copy_len = size;
    size_t offset = copy_len - overlap_amount;

    // Forward overlap: dst > src, overlap by overlap_amount bytes
    // src = buf, dst = buf + offset
    if (offset > 0 && offset + copy_len <= bufsize)
    {
      // Fill buffer with pattern
      for (size_t i = 0; i < bufsize; ++i)
        buf[i] = static_cast<unsigned char>(i & 0xff);
      // Save reference copy of source before move
      for (size_t i = 0; i < copy_len; ++i)
        ref[i] = buf[i];

      void* ret = my_memmove(buf + offset, buf, copy_len);
      EXPECT(ret == buf + offset, "Forward memmove return value should be dst");
      for (size_t i = 0; i < copy_len; ++i)
      {
        EXPECT(
          buf[offset + i] == ref[i],
          "Forward memmove mismatch at index {}, size {}, overlap {}",
          i,
          size,
          overlap_amount);
      }
    }

    // Backward overlap: dst < src, overlap by overlap_amount bytes
    // src = buf + offset, dst = buf
    if (offset > 0 && offset + copy_len <= bufsize)
    {
      // Re-fill buffer with pattern
      for (size_t i = 0; i < bufsize; ++i)
        buf[i] = static_cast<unsigned char>(i & 0xff);
      // Save reference of source (which starts at offset)
      for (size_t i = 0; i < copy_len; ++i)
        ref[i] = buf[offset + i];

      void* ret = my_memmove(buf, buf + offset, copy_len);
      EXPECT(ret == buf, "Backward memmove return value should be dst");
      for (size_t i = 0; i < copy_len; ++i)
      {
        EXPECT(
          buf[i] == ref[i],
          "Backward memmove mismatch at index {}, size {}, overlap {}",
          i,
          size,
          overlap_amount);
      }
    }
  }

  // Non-overlapping memmove should work like memcpy
  {
    auto* s = static_cast<unsigned char*>(my_malloc(size + 1));
    auto* d = static_cast<unsigned char*>(my_malloc(size + 1));
    for (size_t i = 0; i < size; ++i)
      s[i] = static_cast<unsigned char>(i & 0xff);
    for (size_t i = 0; i < size; ++i)
      d[i] = 0;

    void* ret = my_memmove(d, s, size);
    EXPECT(ret == d, "Non-overlapping memmove return value should be dst");
    for (size_t i = 0; i < size; ++i)
    {
      EXPECT(
        d[i] == static_cast<unsigned char>(i & 0xff),
        "Non-overlapping memmove mismatch at {}",
        i);
    }
    my_free(s);
    my_free(d);
  }

  // Zero-length and dst==src edge cases
  {
    auto* p = static_cast<unsigned char*>(my_malloc(size + 1));
    for (size_t i = 0; i < size; ++i)
      p[i] = static_cast<unsigned char>(i & 0xff);

    // Zero-length should be a no-op
    void* ret = my_memmove(p, p + 1, 0);
    EXPECT(ret == p, "Zero-length memmove should return dst");

    // dst == src should be a no-op
    if (size > 0)
    {
      ret = my_memmove(p, p, size);
      EXPECT(ret == p, "dst==src memmove should return dst");
      for (size_t i = 0; i < size; ++i)
      {
        EXPECT(
          p[i] == static_cast<unsigned char>(i & 0xff),
          "dst==src memmove should not corrupt data");
      }
    }
    my_free(p);
  }

  my_free(buf);
  my_free(ref);
}

/**
 * Exhaustive overlap test: for a given buffer size, test memmove for
 * every possible (offset, copy_len) combination within the buffer.
 * This catches subtle off-by-one and alignment issues.
 */
void check_exhaustive_overlaps(size_t bufsize)
{
  START_TEST("exhaustive memmove overlaps for bufsize {}", bufsize);

  auto* buf = static_cast<unsigned char*>(my_malloc(bufsize));
  auto* ref = static_cast<unsigned char*>(my_malloc(bufsize));

  for (size_t offset = 1; offset < bufsize; offset++)
  {
    for (size_t copy_len = 1; copy_len + offset <= bufsize; copy_len++)
    {
      // Forward overlap test: dst > src
      for (size_t i = 0; i < bufsize; ++i)
        buf[i] = static_cast<unsigned char>((i * 7 + 13) & 0xff);
      for (size_t i = 0; i < copy_len; ++i)
        ref[i] = buf[i];

      my_memmove(buf + offset, buf, copy_len);
      for (size_t i = 0; i < copy_len; ++i)
      {
        EXPECT(
          buf[offset + i] == ref[i],
          "Exhaustive fwd mismatch: bufsize {}, offset {}, len {}, idx {}",
          bufsize,
          offset,
          copy_len,
          i);
      }

      // Backward overlap test: dst < src
      for (size_t i = 0; i < bufsize; ++i)
        buf[i] = static_cast<unsigned char>((i * 7 + 13) & 0xff);
      for (size_t i = 0; i < copy_len; ++i)
        ref[i] = buf[offset + i];

      my_memmove(buf, buf + offset, copy_len);
      for (size_t i = 0; i < copy_len; ++i)
      {
        EXPECT(
          buf[i] == ref[i],
          "Exhaustive bwd mismatch: bufsize {}, offset {}, len {}, idx {}",
          bufsize,
          offset,
          copy_len,
          i);
      }
    }
  }

  my_free(buf);
  my_free(ref);
}

/**
 * Test memmove at alignment boundary sizes.  These sizes are chosen to
 * exercise transitions between different code paths in the Arch
 * implementations (small_copies, block_copy, rep movsb thresholds).
 */
void check_alignment_boundary_overlaps()
{
  START_TEST("memmove alignment boundary tests");

  // Sizes near Arch path thresholds
  static const size_t boundary_sizes[] = {
    1,   2,   3,   4,   7,   8,    9,    15,   16,   17,  31,
    32,  33,  48,  63,  64,  65,   127,  128,  129,  255, 256,
    257, 511, 512, 513, 768, 1023, 1024, 1025, 2048, 4096};

  for (auto size : boundary_sizes)
  {
    size_t bufsize = 2 * size + 64;
    auto* buf = static_cast<unsigned char*>(my_malloc(bufsize));
    auto* ref = static_cast<unsigned char*>(my_malloc(bufsize));

    // Test with 1-byte offset (maximum overlap)
    for (size_t i = 0; i < bufsize; ++i)
      buf[i] = static_cast<unsigned char>((i * 11 + 3) & 0xff);
    for (size_t i = 0; i < size; ++i)
      ref[i] = buf[i];
    my_memmove(buf + 1, buf, size);
    for (size_t i = 0; i < size; ++i)
    {
      EXPECT(
        buf[1 + i] == ref[i],
        "Boundary fwd+1 mismatch at size {}, idx {}",
        size,
        i);
    }

    // Test with 1-byte offset backward
    for (size_t i = 0; i < bufsize; ++i)
      buf[i] = static_cast<unsigned char>((i * 11 + 3) & 0xff);
    for (size_t i = 0; i < size; ++i)
      ref[i] = buf[1 + i];
    my_memmove(buf, buf + 1, size);
    for (size_t i = 0; i < size; ++i)
    {
      EXPECT(
        buf[i] == ref[i],
        "Boundary bwd+1 mismatch at size {}, idx {}",
        size,
        i);
    }

    // Test with pointer-sized offset (alignof(void*))
    size_t ptr_off = sizeof(void*);
    if (ptr_off < size)
    {
      for (size_t i = 0; i < bufsize; ++i)
        buf[i] = static_cast<unsigned char>((i * 11 + 3) & 0xff);
      for (size_t i = 0; i < size; ++i)
        ref[i] = buf[i];
      my_memmove(buf + ptr_off, buf, size);
      for (size_t i = 0; i < size; ++i)
      {
        EXPECT(
          buf[ptr_off + i] == ref[i],
          "Boundary fwd+ptr mismatch at size {}, idx {}",
          size,
          i);
      }

      for (size_t i = 0; i < bufsize; ++i)
        buf[i] = static_cast<unsigned char>((i * 11 + 3) & 0xff);
      for (size_t i = 0; i < size; ++i)
        ref[i] = buf[ptr_off + i];
      my_memmove(buf, buf + ptr_off, size);
      for (size_t i = 0; i < size; ++i)
      {
        EXPECT(
          buf[i] == ref[i],
          "Boundary bwd+ptr mismatch at size {}, idx {}",
          size,
          i);
      }
    }

    // Test with misaligned start (offset 3 from allocation start)
    if (size + 3 + size <= bufsize)
    {
      for (size_t i = 0; i < bufsize; ++i)
        buf[i] = static_cast<unsigned char>((i * 11 + 3) & 0xff);
      for (size_t i = 0; i < size; ++i)
        ref[i] = buf[3 + i];
      my_memmove(buf + 3 + 1, buf + 3, size);
      for (size_t i = 0; i < size; ++i)
      {
        EXPECT(
          buf[3 + 1 + i] == ref[i],
          "Boundary misaligned fwd mismatch at size {}, idx {}",
          size,
          i);
      }
    }

    my_free(buf);
    my_free(ref);
  }
}

/**
 * Test memmove via direct snmalloc::memmove<false> (unchecked) to exercise
 * arch-specific code paths without bounds checking interference.
 */
void check_direct_memmove(size_t size)
{
  START_TEST("direct snmalloc::memmove<false> for size {}", size);

  size_t bufsize = 2 * size + 1;
  auto* buf = static_cast<unsigned char*>(my_malloc(bufsize));
  auto* ref = static_cast<unsigned char*>(my_malloc(bufsize));

  // Forward overlap (dst > src) with offset 1
  if (size > 0)
  {
    for (size_t i = 0; i < bufsize; ++i)
      buf[i] = static_cast<unsigned char>(i & 0xff);
    for (size_t i = 0; i < size; ++i)
      ref[i] = buf[i];

    snmalloc::memmove<false>(buf + 1, buf, size);
    for (size_t i = 0; i < size; ++i)
    {
      EXPECT(
        buf[1 + i] == ref[i],
        "Direct fwd mismatch at size {}, idx {}",
        size,
        i);
    }
  }

  // Reverse overlap (dst < src) with offset 1
  if (size > 0)
  {
    for (size_t i = 0; i < bufsize; ++i)
      buf[i] = static_cast<unsigned char>(i & 0xff);
    for (size_t i = 0; i < size; ++i)
      ref[i] = buf[1 + i];

    snmalloc::memmove<false>(buf, buf + 1, size);
    for (size_t i = 0; i < size; ++i)
    {
      EXPECT(
        buf[i] == ref[i], "Direct bwd mismatch at size {}, idx {}", size, i);
    }
  }

  my_free(buf);
  my_free(ref);
}

void check_memmove_bounds(size_t size, size_t out_of_bounds)
{
  START_TEST(
    "memmove bounds, size {}, {} bytes out of bounds", size, out_of_bounds);
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
    my_memmove(d, s, size + out_of_bounds);
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
  static constexpr size_t min_class_size =
    sizeclass_to_size(size_to_sizeclass(MIN_ALLOC_SIZE));

  std::initializer_list<size_t> sizes = {min_class_size, 1024, 2 * 1024 * 1024};
  static_assert(
    MIN_ALLOC_SIZE < 1024,
    "Can't detect overflow except at sizeclass boundaries");

  for (auto sz : sizes)
  {
    // Check in bounds
    check_memmove_bounds(sz, 0);
    // Check one byte out
    check_memmove_bounds(sz, 1);
    // Check one object out of bounds
    check_memmove_bounds(sz, sz);
  }
  // Test memmove with various sizes covering all Arch path thresholds
  for (size_t x = 1; x < 2048; x++)
  {
    check_overlaps(x);
  }
  // Exhaustive overlap testing for small sizes (every offset * length combo)
  for (size_t x = 2; x <= 64; x++)
  {
    check_exhaustive_overlaps(x);
  }
  // Alignment boundary tests
  check_alignment_boundary_overlaps();
  // Direct snmalloc::memmove<false> tests (unchecked path)
  for (size_t x = 0; x < 2048; x++)
  {
    check_direct_memmove(x);
  }
}
#endif
