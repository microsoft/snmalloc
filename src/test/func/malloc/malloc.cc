#include <stdio.h>
#include <test/helpers.h>
#include <test/setup.h>

#define SNMALLOC_NAME_MANGLE(a) our_##a
#undef SNMALLOC_NO_REALLOCARRAY
#undef SNMALLOC_NO_REALLOCARR
#define SNMALLOC_BOOTSTRAP_ALLOCATOR
#include <snmalloc/override/malloc.cc>

using namespace snmalloc;

constexpr int SUCCESS = 0;

void check_result(size_t size, size_t align, void* p, int err, bool null)
{
  bool failed = false;
  EXPECT((errno == err), "Expected error: {} but got {}", err, errno);
  if (null)
  {
    EXPECT(p == nullptr, "Expected null but got {}", p);
    return;
  }

  if ((p == nullptr) && (size != 0))
  {
    INFO("Unexpected null returned.\n");
    failed = true;
  }
  const auto alloc_size = our_malloc_usable_size(p);
  auto expected_size = our_malloc_good_size(size);
  const auto exact_size = align == 1;
#ifdef __CHERI_PURE_CAPABILITY__
  const auto cheri_size = __builtin_cheri_length_get(p);
  if (cheri_size != alloc_size && (size != 0))
  {
    INFO("Cheri size is {}, but required to be {}.", cheri_size, alloc_size);
    failed = true;
  }
#  if defined(CHERI_PERM_SW_VMEM)
  const auto cheri_perms = __builtin_cheri_perms_get(p);
  if (cheri_perms & CHERI_PERM_SW_VMEM)
  {
    INFO("Cheri permissions include VMEM authority");
    failed = true;
  }
#  endif
  if (p != nullptr)
  {
    /*
     * Scan the allocation for any tagged capabilities. Since this test doesn't
     * use the allocated memory if there is a valid cap it must have leaked from
     * the allocator, which is bad.
     */
    void** vp = static_cast<void**>(p);
    for (size_t n = 0; n < alloc_size / sizeof(*vp); vp++, n++)
    {
      void* c = *vp;
      if (__builtin_cheri_tag_get(c))
      {
        printf("Found cap tag set in alloc: %#p at %#p\n", c, vp);
        failed = true;
      }
    }
  }
#endif
  if (exact_size && (alloc_size != expected_size) && (size != 0))
  {
    INFO(
      "Usable size is {}, but required to be {}.", alloc_size, expected_size);
    failed = true;
  }
  if ((!exact_size) && (alloc_size < expected_size))
  {
    INFO(
      "Usable size is {}, but required to be at least {}.",
      alloc_size,
      expected_size);
    failed = true;
  }
  if (((address_cast(p) % align) != 0) && (size != 0))
  {
    INFO("Address is {}, but required to be aligned to {}.\n", p, align);
    failed = true;
  }
  if ((address_cast(p) % natural_alignment(size)) != 0)
  {
    INFO(
      "Address is {}, but should have natural alignment to {}.\n",
      p,
      natural_alignment(size));
    failed = true;
  }

  EXPECT(!failed, "check_result failed! {}", p);
  our_free(p);
}

void test_calloc(size_t nmemb, size_t size, int err, bool null)
{
  START_TEST("calloc({}, {})  combined size {}\n", nmemb, size, nmemb * size);
  errno = SUCCESS;
  void* p = our_calloc(nmemb, size);

  if (p != nullptr)
  {
    for (size_t i = 0; i < (size * nmemb); i++)
    {
      EXPECT(((uint8_t*)p)[i] == 0, "non-zero at {}", i);
    }
  }
  check_result(nmemb * size, 1, p, err, null);
}

void test_realloc(void* p, size_t size, int err, bool null)
{
  size_t old_size = 0;
  if (p != nullptr)
    old_size = our_malloc_usable_size(p);

  START_TEST("realloc({}({}), {})", p, old_size, size);
  errno = SUCCESS;
  auto new_p = our_realloc(p, size);
  check_result(size, 1, new_p, err, null);
  // Realloc failure case, deallocate original block as not
  // handled by check_result.
  if (new_p == nullptr && size != 0)
    our_free(p);
}

void test_posix_memalign(size_t size, size_t align, int err, bool null)
{
  START_TEST("posix_memalign(&p, {}, {})", align, size);
  void* p = nullptr;
  errno = our_posix_memalign(&p, align, size);
  check_result(size, align, p, err, null);
}

void test_memalign(size_t size, size_t align, int err, bool null)
{
  START_TEST("memalign({}, {})", align, size);
  errno = SUCCESS;
  void* p = our_memalign(align, size);
  check_result(size, align, p, err, null);
}

void test_reallocarray(void* p, size_t nmemb, size_t size, int err, bool null)
{
  size_t old_size = 0;
  size_t tsize = nmemb * size;
  if (p != nullptr)
    old_size = our_malloc_usable_size(p);

  START_TEST("reallocarray({}({}), {})", p, old_size, tsize);
  errno = SUCCESS;
  auto new_p = our_reallocarray(p, nmemb, size);
  if (new_p == nullptr && tsize != 0)
    our_free(p);
  check_result(tsize, 1, new_p, err, null);
}

void test_reallocarr(
  size_t size_old, size_t nmemb, size_t size, int err, bool null)
{
  void* p = nullptr;

  if (size_old != (size_t)~0)
    p = our_malloc(size_old);
  START_TEST("reallocarr({}({}), {})", p, nmemb, size);
  errno = SUCCESS;
  int r = our_reallocarr(&p, nmemb, size);
  EXPECT(r == err, "reallocarr failed! expected {} got {}\n", err, r);

  check_result(nmemb * size, 1, p, err, null);
  p = our_malloc(size);
  if (!p)
  {
    return;
  }
  for (size_t i = 1; i < size; i++)
    static_cast<char*>(p)[i] = 1;
  our_reallocarr(&p, nmemb, size);
  if (r != SUCCESS)
    our_free(p);

  for (size_t i = 1; i < size; i++)
  {
    EXPECT(static_cast<char*>(p)[i] == 1, "data consistency failed! at {}", i);
  }
  our_free(p);
}

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);

  setup();

  // Smoke test the fatal error builder.  Check that it can generate strings
  // including all of the kinds of things that it expects to be able to format.
  //
  // Note: We cannot use the check or assert macros here because they depend on
  // `MessageBuilder` working.  They are safe to use in any other test.
  void* fakeptr = unsafe_from_uintptr<void>(static_cast<uintptr_t>(0x42));
  MessageBuilder<1024> b{
    "testing pointer {} size_t {} message, {} world, null is {}, -123456 is "
    "{}, 1234567 is {}",
    fakeptr,
    size_t(42),
    "hello",
    nullptr,
    -123456,
    1234567};
  if (
    strcmp(
      "testing pointer 0x42 size_t 0x2a message, hello world, null is "
      "(nullptr), "
      "-123456 is -123456, 1234567 is 1234567",
      b.get_message()) != 0)
  {
    printf("Incorrect rendering of fatal error message: %s\n", b.get_message());
    abort();
  }

  our_free(nullptr);

  /* A very large allocation size that we expect to fail. */
  const size_t too_big_size = ((size_t)-1) / 2;
  check_result(too_big_size, 1, our_malloc(too_big_size), ENOMEM, true);
  errno = SUCCESS;

  for (smallsizeclass_t sc = 0; sc < (MAX_SMALL_SIZECLASS_BITS + 4); sc++)
  {
    const size_t size = bits::one_at_bit(sc);
    START_TEST("malloc: {}", size);
    errno = SUCCESS;
    check_result(size, 1, our_malloc(size), SUCCESS, false);
    errno = SUCCESS;
    check_result(size + 1, 1, our_malloc(size + 1), SUCCESS, false);
  }

  test_calloc(0, 0, SUCCESS, false);

  our_free(nullptr);

  test_calloc(1, too_big_size, ENOMEM, true);
  errno = SUCCESS;

  for (smallsizeclass_t sc = 0; sc < NUM_SMALL_SIZECLASSES; sc++)
  {
    const size_t size = sizeclass_to_size(sc);

    bool overflow = false;
    for (size_t n = 1;
         bits::umul(size, n, overflow) <= MAX_SMALL_SIZECLASS_SIZE;
         n *= 5)
    {
      if (overflow)
        break;

      test_calloc(n, size, SUCCESS, false);
      test_calloc(n, 0, SUCCESS, false);
    }
    test_calloc(0, size, SUCCESS, false);
  }

  // Check realloc(nullptr,0) behaves like malloc(1)
  test_realloc(nullptr, 0, SUCCESS, false);

  for (smallsizeclass_t sc = 0; sc < NUM_SMALL_SIZECLASSES; sc++)
  {
    const size_t size = sizeclass_to_size(sc);
    test_realloc(our_malloc(size), size, SUCCESS, false);
    test_realloc(nullptr, size, SUCCESS, false);
    test_realloc(our_malloc(size), too_big_size, ENOMEM, true);
    for (smallsizeclass_t sc2 = 0; sc2 < NUM_SMALL_SIZECLASSES; sc2++)
    {
      const size_t size2 = sizeclass_to_size(sc2);
      test_realloc(our_malloc(size), size2, SUCCESS, false);
      test_realloc(our_malloc(size + 1), size2, SUCCESS, false);
    }
    // Check realloc(p,0), behaves like free(p), if p != nullptr
    test_realloc(our_malloc(size), 0, SUCCESS, true);
  }

  for (smallsizeclass_t sc = 0; sc < (MAX_SMALL_SIZECLASS_BITS + 4); sc++)
  {
    const size_t size = bits::one_at_bit(sc);
    test_realloc(our_malloc(size), size, SUCCESS, false);
    test_realloc(nullptr, size, SUCCESS, false);
    test_realloc(our_malloc(size), too_big_size, ENOMEM, true);
    for (smallsizeclass_t sc2 = 0; sc2 < (MAX_SMALL_SIZECLASS_BITS + 4); sc2++)
    {
      const size_t size2 = bits::one_at_bit(sc2);
      INFO("size1: {}, size2:{}\n", size, size2);
      test_realloc(our_malloc(size), size2, SUCCESS, false);
      test_realloc(our_malloc(size + 1), size2, SUCCESS, false);
    }
  }

  test_realloc(our_malloc(64), 4194304, SUCCESS, false);

  test_posix_memalign(0, 0, EINVAL, true);
  test_posix_memalign(too_big_size, 0, EINVAL, true);
  test_posix_memalign(OS_PAGE_SIZE, sizeof(uintptr_t) / 2, EINVAL, true);

  for (size_t align = sizeof(uintptr_t); align < MAX_SMALL_SIZECLASS_SIZE * 8;
       align <<= 1)
  {
    // Check overflow with alignment taking it round to 0.
    test_memalign(1 - align, align, ENOMEM, true);

    for (smallsizeclass_t sc = 0; sc < NUM_SMALL_SIZECLASSES - 6; sc++)
    {
      const size_t size = sizeclass_to_size(sc);
      test_posix_memalign(size, align, SUCCESS, false);
      test_posix_memalign(size, 0, EINVAL, true);
      test_memalign(size, align, SUCCESS, false);
    }
    test_posix_memalign(0, align, SUCCESS, false);
    test_posix_memalign(too_big_size, align, ENOMEM, true);
    test_posix_memalign(0, align + 1, EINVAL, true);
  }

  test_reallocarray(nullptr, 1, 0, SUCCESS, false);
  for (smallsizeclass_t sc = 0; sc < (MAX_SMALL_SIZECLASS_BITS + 4); sc++)
  {
    const size_t size = bits::one_at_bit(sc);
    test_reallocarray(our_malloc(size), 1, size, SUCCESS, false);
    test_reallocarray(our_malloc(size), 1, 0, SUCCESS, false);
    test_reallocarray(nullptr, 1, size, SUCCESS, false);
    test_reallocarray(our_malloc(size), 1, too_big_size, ENOMEM, true);
    for (smallsizeclass_t sc2 = 0; sc2 < (MAX_SMALL_SIZECLASS_BITS + 4); sc2++)
    {
      const size_t size2 = bits::one_at_bit(sc2);
      test_reallocarray(our_malloc(size), 1, size2, SUCCESS, false);
      test_reallocarray(our_malloc(size + 1), 1, size2, SUCCESS, false);
    }
  }

  test_reallocarr((size_t)~0, 1, 0, SUCCESS, false);
  test_reallocarr((size_t)~0, 1, 16, SUCCESS, false);

  for (smallsizeclass_t sc = 0; sc < (MAX_SMALL_SIZECLASS_BITS + 4); sc++)
  {
    const size_t size = bits::one_at_bit(sc);
    test_reallocarr(size, 1, size, SUCCESS, false);
    test_reallocarr(size, 1, 0, SUCCESS, false);
    test_reallocarr(size, 2, size, SUCCESS, false);
    void* p = our_malloc(size);
    EXPECT(p != nullptr, "realloc alloc failed with {}", size);
    int r = our_reallocarr(&p, 1, too_big_size);
    EXPECT(r == ENOMEM, "expected failure on allocation\n");
    our_free(p);

    for (smallsizeclass_t sc2 = 0; sc2 < (MAX_SMALL_SIZECLASS_BITS + 4); sc2++)
    {
      const size_t size2 = bits::one_at_bit(sc2);
      START_TEST("size1: {}, size2:{}", size, size2);
      test_reallocarr(size, 1, size2, SUCCESS, false);
    }
  }

  EXPECT(
    our_malloc_usable_size(nullptr) == 0,
    "malloc_usable_size(nullptr) should be zero");

  snmalloc::debug_check_empty();
  return 0;
}
