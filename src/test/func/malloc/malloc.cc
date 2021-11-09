#include <stdio.h>
#include <test/setup.h>

#define SNMALLOC_NAME_MANGLE(a) our_##a
#include "../../../override/malloc.cc"

using namespace snmalloc;

constexpr int SUCCESS = 0;

void check_result(size_t size, size_t align, void* p, int err, bool null)
{
  bool failed = false;
  if (errno != err && err != SUCCESS)
  {
    printf("Expected error: %d but got %d\n", err, errno);
    failed = true;
  }

  if (null)
  {
    if (p != nullptr)
    {
      printf("Expected null, and got non-null return!\n");
      abort();
    }
    return;
  }

  if ((p == nullptr) && (size != 0))
  {
    printf("Unexpected null returned.\n");
    failed = true;
  }
  const auto alloc_size = our_malloc_usable_size(p);
  const auto expected_size = round_size(size);
#ifdef SNMALLOC_PASS_THROUGH
  // Calling system allocator may allocate a larger block than
  // snmalloc. Note, we have called the system allocator with
  // the size snmalloc would allocate, so it won't be smaller.
  const auto exact_size = false;
#else
  const auto exact_size = align == 1;
#endif
  if (exact_size && (alloc_size != expected_size) && (size != 0))
  {
    printf(
      "Usable size is %zu, but required to be %zu.\n",
      alloc_size,
      expected_size);
    failed = true;
  }
  if ((!exact_size) && (alloc_size < expected_size))
  {
    printf(
      "Usable size is %zu, but required to be at least %zu.\n",
      alloc_size,
      expected_size);
    failed = true;
  }
  if (
    (static_cast<size_t>(reinterpret_cast<uintptr_t>(p) % align) != 0) &&
    (size != 0))
  {
    printf(
      "Address is 0x%zx, but required to be aligned to 0x%zx.\n",
      reinterpret_cast<size_t>(p),
      align);
    failed = true;
  }
  if (
    static_cast<size_t>(
      reinterpret_cast<uintptr_t>(p) % natural_alignment(size)) != 0)
  {
    printf(
      "Address is 0x%zx, but should have natural alignment to 0x%zx.\n",
      reinterpret_cast<size_t>(p),
      natural_alignment(size));
    failed = true;
  }

  if (failed)
  {
    printf("check_result failed! %p", p);
    abort();
  }
  our_free(p);
}

void test_calloc(size_t nmemb, size_t size, int err, bool null)
{
  printf("calloc(%zu, %zu)  combined size %zu\n", nmemb, size, nmemb * size);
  errno = SUCCESS;
  void* p = our_calloc(nmemb, size);

  if (p != nullptr)
  {
    for (size_t i = 0; i < (size * nmemb); i++)
    {
      if (((uint8_t*)p)[i] != 0)
      {
        printf("non-zero at @%zu\n", i);
        abort();
      }
    }
  }
  check_result(nmemb * size, 1, p, err, null);
}

void test_realloc(void* p, size_t size, int err, bool null)
{
  size_t old_size = 0;
  if (p != nullptr)
    old_size = our_malloc_usable_size(p);

  printf("realloc(%p(%zu), %zu)\n", p, old_size, size);
  errno = SUCCESS;
  auto new_p = our_realloc(p, size);
  // Realloc failure case, deallocate original block
  if (new_p == nullptr && size != 0)
    our_free(p);
  check_result(size, 1, new_p, err, null);
}

void test_posix_memalign(size_t size, size_t align, int err, bool null)
{
  printf("posix_memalign(&p, %zu, %zu)\n", align, size);
  void* p = nullptr;
  errno = our_posix_memalign(&p, align, size);
  check_result(size, align, p, err, null);
}

void test_memalign(size_t size, size_t align, int err, bool null)
{
  printf("memalign(%zu, %zu)\n", align, size);
  errno = SUCCESS;
  void* p = our_memalign(align, size);
  check_result(size, align, p, err, null);
}

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);

  setup();

  our_free(nullptr);

  for (smallsizeclass_t sc = 0; sc < (MAX_SMALL_SIZECLASS_BITS + 4); sc++)
  {
    const size_t size = bits::one_at_bit(sc);
    printf("malloc: %zu\n", size);
    errno = SUCCESS;
    check_result(size, 1, our_malloc(size), SUCCESS, false);
    errno = SUCCESS;
    check_result(size + 1, 1, our_malloc(size + 1), SUCCESS, false);
  }

  test_calloc(0, 0, SUCCESS, false);

  our_free(nullptr);

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

  for (smallsizeclass_t sc = 0; sc < NUM_SMALL_SIZECLASSES; sc++)
  {
    const size_t size = sizeclass_to_size(sc);
    test_realloc(our_malloc(size), size, SUCCESS, false);
    test_realloc(nullptr, size, SUCCESS, false);
    test_realloc(our_malloc(size), ((size_t)-1) / 2, ENOMEM, true);
    for (smallsizeclass_t sc2 = 0; sc2 < NUM_SMALL_SIZECLASSES; sc2++)
    {
      const size_t size2 = sizeclass_to_size(sc2);
      test_realloc(our_malloc(size), size2, SUCCESS, false);
      test_realloc(our_malloc(size + 1), size2, SUCCESS, false);
    }
  }

  for (smallsizeclass_t sc = 0; sc < (MAX_SMALL_SIZECLASS_BITS + 4); sc++)
  {
    const size_t size = bits::one_at_bit(sc);
    test_realloc(our_malloc(size), size, SUCCESS, false);
    test_realloc(nullptr, size, SUCCESS, false);
    test_realloc(our_malloc(size), ((size_t)-1) / 2, ENOMEM, true);
    for (smallsizeclass_t sc2 = 0; sc2 < (MAX_SMALL_SIZECLASS_BITS + 4); sc2++)
    {
      const size_t size2 = bits::one_at_bit(sc2);
      printf("size1: %zu, size2:%zu\n", size, size2);
      test_realloc(our_malloc(size), size2, SUCCESS, false);
      test_realloc(our_malloc(size + 1), size2, SUCCESS, false);
    }
  }

  test_realloc(our_malloc(64), 4194304, SUCCESS, false);

  test_posix_memalign(0, 0, EINVAL, true);
  test_posix_memalign(((size_t)-1) / 2, 0, EINVAL, true);
  test_posix_memalign(OS_PAGE_SIZE, sizeof(uintptr_t) / 2, EINVAL, true);

  for (size_t align = sizeof(uintptr_t); align < MAX_SMALL_SIZECLASS_SIZE * 8;
       align <<= 1)
  {
    for (smallsizeclass_t sc = 0; sc < NUM_SMALL_SIZECLASSES - 6; sc++)
    {
      const size_t size = sizeclass_to_size(sc);
      test_posix_memalign(size, align, SUCCESS, false);
      test_posix_memalign(size, 0, EINVAL, true);
      test_memalign(size, align, SUCCESS, false);
    }
    test_posix_memalign(0, align, SUCCESS, false);
    test_posix_memalign(((size_t)-1) / 2, align, ENOMEM, true);
    test_posix_memalign(0, align + 1, EINVAL, true);
  }

  if (our_malloc_usable_size(nullptr) != 0)
  {
    printf("malloc_usable_size(nullptr) should be zero");
    abort();
  }

  snmalloc::debug_check_empty<snmalloc::Globals>();
  return 0;
}
