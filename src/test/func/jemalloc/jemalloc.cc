#include <functional>
#include <limits.h>
#include <stdio.h>
#include <test/helpers.h>
#include <test/setup.h>

#define SNMALLOC_NAME_MANGLE(a) our_##a
#undef SNMALLOC_NO_REALLOCARRAY
#undef SNMALLOC_NO_REALLOCARR
#define SNMALLOC_BOOTSTRAP_ALLOCATOR
#define SNMALLOC_JEMALLOC3_EXPERIMENTAL
#define SNMALLOC_JEMALLOC_NONSTANDARD
#include <snmalloc/override/jemalloc_compat.cc>
#include <snmalloc/override/malloc.cc>

#if __has_include(<malloc_np.h>)
#  include <malloc_np.h>
#endif

#ifdef __FreeBSD__
/**
 * Enable testing against the versions that we get from libc or elsewhere.
 * Enabled by default on FreeBSD where all of the jemalloc functions are
 * exported from libc.
 */
#  define TEST_JEMALLOC_MALLOCX
#endif

#define OUR_MALLOCX_LG_ALIGN(la) (static_cast<int>(la))
#define OUR_MALLOCX_ZERO (one_at_bit<int>(6))

#define OUR_ALLOCM_NO_MOVE (one_at_bit<int>(7))

#define OUR_ALLOCM_SUCCESS 0
#define OUR_ALLOCM_ERR_OOM 1
#define OUR_ALLOCM_ERR_NOT_MOVED 2

#ifndef MALLOCX_LG_ALIGN
#  define MALLOCX_LG_ALIGN(la) OUR_MALLOCX_LG_ALIGN(la)
#endif
#ifndef MALLOCX_ZERO
#  define MALLOCX_ZERO OUR_MALLOCX_ZERO
#endif

#ifndef ALLOCM_LG_ALIGN
#  define ALLOCM_LG_ALIGN(la) OUR_MALLOCX_LG_ALIGN(la)
#endif
#ifndef ALLOCM_ZERO
#  define ALLOCM_ZERO OUR_MALLOCX_ZERO
#endif
#ifndef ALLOCM_NO_MOVE
#  define ALLOCM_NO_MOVE OUR_ALLOCM_NO_MOVE
#endif
#ifndef ALLOCM_SUCCESS
#  define ALLOCM_SUCCESS OUR_ALLOCM_SUCCESS
#endif
#ifndef ALLOCM_ERR_OOM
#  define ALLOCM_ERR_OOM OUR_ALLOCM_ERR_OOM
#endif
#ifndef ALLOCM_ERR_NOT_MOVED
#  define ALLOCM_ERR_NOT_MOVED OUR_ALLOCM_ERR_NOT_MOVED
#endif

using namespace snmalloc;
using namespace snmalloc::bits;

namespace
{
  /**
   * Test whether the MALLOCX_LG_ALIGN macro is defined correctly.  This test
   * will pass trivially if we don't have the malloc_np.h header from
   * jemalloc, but at least the FreeBSD action runners in CI do have this
   * header.
   */
  template<int Size>
  void check_lg_align_macro()
  {
    static_assert(
      OUR_MALLOCX_LG_ALIGN(Size) == MALLOCX_LG_ALIGN(Size),
      "Our definition of MALLOCX_LG_ALIGN is wrong");
    static_assert(
      OUR_MALLOCX_LG_ALIGN(Size) == ALLOCM_LG_ALIGN(Size),
      "Our definition of ALLOCM_LG_ALIGN is wrong");
    static_assert(
      JEMallocFlags(Size).log2align() == Size, "Out log2 align mask is wrong");
    if constexpr (Size > 0)
    {
      check_lg_align_macro<Size - 1>();
    }
  }

  /**
   * The default maximum number of bits of address space to use for tests.
   * This is clamped on platforms without lazy commit because this much RAM
   * (or, at least, commit charge) will be used on such systems.
   *
   * Thread sanitizer makes these tests *very* slow, so reduce the size
   * significantly when it's enabled.
   */
  constexpr size_t DefaultMax = 22;

  /**
   * Run a test with a range of sizes and alignments.  The `test` argument is
   * called with a size and log2 alignment as parameters.
   */
  template<size_t Log2MaxSize = DefaultMax>
  void test_sizes_and_alignments(std::function<void(size_t, int)> test)
  {
    constexpr size_t low = 5;
    for (size_t base = low; base < Log2MaxSize; base++)
    {
      INFO("\tTrying {}-byte allocations\n", one_at_bit(base));
      for (size_t i = 0; i < one_at_bit(low); i++)
      {
        for (int align = 1; align < 20; align++)
        {
          test(one_at_bit(base) + (i << (base - low)), align);
        }
      }
    }
  }

  /**
   * Test that the size reported by nallocx corresponds to the size reported by
   * sallocx on the return value from mallocx.
   */
  template<
    void*(Mallocx)(size_t, int),
    void(Dallocx)(void*, int),
    size_t(Sallocx)(const void*, int),
    size_t(Nallocx)(size_t, int)>
  void test_size()
  {
    START_TEST("nallocx and mallocx return the same size");
    test_sizes_and_alignments([](size_t size, int align) {
      int flags = MALLOCX_LG_ALIGN(align);
      size_t expected = Nallocx(size, flags);
      void* ptr = Mallocx(size, flags);
      EXPECT(
        ptr != nullptr,
        "Failed to allocate {} bytes with {}-bit alignment",
        size,
        align);
      size_t allocated = Sallocx(ptr, 0);
      EXPECT(
        allocated == expected,
        "Expected to have allocated {} bytes, got {} bytes",
        expected,
        allocated);
      Dallocx(ptr, 0);
    });
  }

  /**
   * Test that, when we request zeroing in rallocx, we get zeroed memory.
   */
  template<
    void*(Mallocx)(size_t, int),
    void(Dallocx)(void*, int),
    void*(Rallocx)(void*, size_t, int)>
  void test_zeroing()
  {
    START_TEST("rallocx can zero the remaining space.");
    // The Rallocx call will copy everything in the first malloc, so stay
    // fairly small.
    auto test = [](size_t size, int align) {
      int flags = MALLOCX_LG_ALIGN(align) | MALLOCX_ZERO;
      char* ptr = static_cast<char*>(Mallocx(size, flags));
      ptr = static_cast<char*>(Rallocx(ptr, size * 2, flags));
      EXPECT(
        ptr != nullptr,
        "Failed to reallocate for {} byte allocation",
        size * 2);
      EXPECT(
        ptr[size] == 0,
        "Memory not zero initialised for {} byte reallocation from {} with "
        "align {}"
        "byte allocation",
        size * 2,
        size,
        align);
      // The second time we run this test, we if we're allocating from a free
      // list then we will reuse this, so make sure it requires explicit
      // zeroing.
      ptr[size] = 12;
      Dallocx(ptr, 0);
    };
    test_sizes_and_alignments<22>(test);
    test_sizes_and_alignments<22>(test);
  }

  /**
   * Test that xallocx reports a size that is at least the requested amount.
   */
  template<
    void*(Mallocx)(size_t, int),
    void(Dallocx)(void*, int),
    size_t(Xallocx)(void*, size_t, size_t, int)>
  void test_xallocx()
  {
    START_TEST("xallocx returns a sensible value.");
    // The Rallocx call will copy all of these, so stay fairly small.
    auto test = [](size_t size, int align) {
      int flags = MALLOCX_LG_ALIGN(align);
      void* ptr = Mallocx(size, flags);
      EXPECT(ptr != nullptr, "Failed to allocate for zx byte allocation", size);
      size_t sz = Xallocx(ptr, size, 1024, flags);
      EXPECT(sz >= size, "xalloc returned {}, expected at least {}", sz, size);
      Dallocx(ptr, 0);
    };
    test_sizes_and_alignments(test);
  }

  template<
    int(Allocm)(void**, size_t*, size_t, int),
    int(Sallocm)(const void*, size_t*, int),
    int(Dallocm)(void*, int),
    int(Nallocm)(size_t*, size_t, int)>
  void test_nallocm_size()
  {
    START_TEST("nallocm and allocm return the same size");
    test_sizes_and_alignments([](size_t size, int align) {
      int flags = ALLOCM_LG_ALIGN(align);
      size_t expected;
      int ret = Nallocm(&expected, size, flags);
      EXPECT(
        (ret == ALLOCM_SUCCESS),
        "nallocm({}, {}) failed with error {}",
        size,
        flags,
        ret);
      void* ptr;
      size_t allocated;
      ret = Allocm(&ptr, &allocated, size, flags);
      EXPECT(
        (ptr != nullptr) && (ret == ALLOCM_SUCCESS),
        "Failed to allocate {} bytes with {} bit alignment",
        size,
        align);
      EXPECT(
        allocated == expected,
        "Expected to have allocated {} bytes, got {} bytes",
        expected,
        allocated);
      ret = Sallocm(ptr, &expected, 0);
      EXPECT(
        (ret == ALLOCM_SUCCESS) && (allocated == expected),
        "Expected to have allocated {} bytes, got {} bytes",
        expected,
        allocated);

      Dallocm(ptr, 0);
    });
  }

  template<
    int(Allocm)(void**, size_t*, size_t, int),
    int(Rallocm)(void**, size_t*, size_t, size_t, int),
    int(Dallocm)(void*, int)>
  void test_rallocm_nomove()
  {
    START_TEST("rallocm non-moving behaviour");
    test_sizes_and_alignments([](size_t size, int align) {
      int flags = ALLOCM_LG_ALIGN(align);
      void* ptr;
      size_t allocated;
      int ret = Allocm(&ptr, &allocated, size, flags);
      void* orig = ptr;
      EXPECT(
        (ptr != nullptr) && (ret == ALLOCM_SUCCESS),
        "Failed to allocate {} bytes with {} bit alignment",
        size,
        align);
      ret = Rallocm(&ptr, nullptr, allocated + 1, 12, flags | ALLOCM_NO_MOVE);
      EXPECT(
        (ret == ALLOCM_ERR_NOT_MOVED) || (ptr == orig),
        "Expected rallocm not to be able to move or reallocate, but return was "
        "{}\n",
        ret);
      Dallocm(ptr, 0);
    });
  }

  template<
    int(Allocm)(void**, size_t*, size_t, int),
    int(Rallocm)(void**, size_t*, size_t, size_t, int),
    int(Sallocm)(const void*, size_t*, int),
    int(Dallocm)(void*, int),
    int(Nallocm)(size_t*, size_t, int)>
  void test_legacy_experimental_apis()
  {
    START_TEST("allocm out-of-memory behaviour");
    void* ptr = nullptr;
    int ret = Allocm(&ptr, nullptr, SIZE_MAX / 2, 0);
    EXPECT(
      (ptr == nullptr) && (ret == OUR_ALLOCM_ERR_OOM),
      "Expected massive allocation to fail with out of memory ({}), received "
      "allocation {}, return code {}",
      OUR_ALLOCM_ERR_OOM,
      ptr,
      ret);
    test_nallocm_size<Allocm, Sallocm, Dallocm, Nallocm>();
    test_rallocm_nomove<Allocm, Rallocm, Dallocm>();
  }
}

int main()
{
  check_lg_align_macro<63>();
  static_assert(
    OUR_MALLOCX_ZERO == MALLOCX_ZERO, "Our MALLOCX_ZERO macro is wrong");
  static_assert(
    OUR_MALLOCX_ZERO == ALLOCM_ZERO, "Our ALLOCM_ZERO macro is wrong");
  static_assert(
    OUR_ALLOCM_NO_MOVE == ALLOCM_NO_MOVE, "Our ALLOCM_NO_MOVE macro is wrong");
  static_assert(
    JEMallocFlags(MALLOCX_ZERO).should_zero(),
    "Our MALLOCX_ZERO is not the value that we are using");
  static_assert(
    !JEMallocFlags(~MALLOCX_ZERO).should_zero(),
    "Our MALLOCX_ZERO is not the value that we are using");
  static_assert(
    JEMallocFlags(ALLOCM_NO_MOVE).may_not_move(),
    "Our ALLOCM_NO_MOVE is not the value that we are using");
  static_assert(
    !JEMallocFlags(~ALLOCM_NO_MOVE).may_not_move(),
    "Our ALLOCM_NO_MOVE is not the value that we are using");
  test_size<our_mallocx, our_dallocx, our_sallocx, our_nallocx>();
  test_zeroing<our_mallocx, our_dallocx, our_rallocx>();
  test_xallocx<our_mallocx, our_dallocx, our_xallocx>();
  test_legacy_experimental_apis<
    our_allocm,
    our_rallocm,
    our_sallocm,
    our_dallocm,
    our_nallocm>();

#ifndef __PIC__
  void* bootstrap = __je_bootstrap_malloc(42);
  if (bootstrap == nullptr)
  {
    printf("Failed to allocate from bootstrap malloc\n");
  }
  __je_bootstrap_free(bootstrap);
#endif

  // These tests are for jemalloc compatibility and so should work with
  // jemalloc's implementation of these functions.  If TEST_JEMALLOC is
  // defined then we try
#ifdef TEST_JEMALLOC_MALLOCX
  test_size<mallocx, dallocx, sallocx, nallocx>();
  test_zeroing<mallocx, dallocx, rallocx>();
  test_xallocx<mallocx, dallocx, xallocx>();
#endif
}
