#include <stdio.h>
#include <test/setup.h>

#define SNMALLOC_NAME_MANGLE(a) our_##a
#include "../../../override/malloc.cc"

using namespace snmalloc;

void check_result(size_t size, size_t align, void* p, int err, bool null)
{
  if (errno != err)
    abort();

  if (null)
  {
    if (p != nullptr)
      abort();

    our_free(p);
    return;
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
  if (exact_size && (alloc_size != expected_size))
  {
    printf(
      "Usable size is %zu, but required to be %zu.\n",
      alloc_size,
      expected_size);
    abort();
  }
  if ((!exact_size) && (alloc_size < expected_size))
  {
    printf(
      "Usable size is %zu, but required to be at least %zu.\n",
      alloc_size,
      expected_size);
    abort();
  }
  if (static_cast<size_t>(reinterpret_cast<uintptr_t>(p) % align) != 0)
  {
    printf(
      "Address is 0x%zx, but required to be aligned to 0x%zx.\n",
      reinterpret_cast<uintptr_t>(p),
      align);
    abort();
  }
  if (
    static_cast<size_t>(
      reinterpret_cast<uintptr_t>(p) % natural_alignment(size)) != 0)
  {
    printf(
      "Address is 0x%zx, but should have natural alignment to 0x%zx.\n",
      reinterpret_cast<uintptr_t>(p),
      natural_alignment(size));
    abort();
  }

  our_free(p);
}

void test_calloc(size_t nmemb, size_t size, int err, bool null)
{
  fprintf(stderr, "calloc(%zu, %zu)\n", nmemb, size);
  errno = 0;
  void* p = our_calloc(nmemb, size);

  if ((p != nullptr) && (errno == 0))
  {
    for (size_t i = 0; i < (size * nmemb); i++)
    {
      if (((uint8_t*)p)[i] != 0)
        abort();
    }
  }
  check_result(nmemb * size, 1, p, err, null);
}

void test_realloc(void* p, size_t size, int err, bool null)
{
  size_t old_size = 0;
  if (p != nullptr)
    old_size = our_malloc_usable_size(p);

  fprintf(stderr, "realloc(%p(%zu), %zu)\n", p, old_size, size);
  errno = 0;
  auto new_p = our_realloc(p, size);
  // Realloc failure case, deallocate original block
  if (new_p == nullptr && size != 0)
    our_free(p);
  check_result(size, 1, new_p, err, null);
}

void test_posix_memalign(size_t size, size_t align, int err, bool null)
{
  fprintf(stderr, "posix_memalign(&p, %zu, %zu)\n", align, size);
  void* p = nullptr;
  errno = our_posix_memalign(&p, align, size);
  check_result(size, align, p, err, null);
}

void test_memalign(size_t size, size_t align, int err, bool null)
{
  fprintf(stderr, "memalign(%zu, %zu)\n", align, size);
  errno = 0;
  void* p = our_memalign(align, size);
  check_result(size, align, p, err, null);
}

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);

  setup();

  constexpr int SUCCESS = 0;

  test_realloc(our_malloc(64), 4194304, SUCCESS, false);

  for (sizeclass_t sc = 0; sc < (SUPERSLAB_BITS + 4); sc++)
  {
    const size_t size = 1ULL << sc;
    printf("malloc: %zu\n", size);
    check_result(size, 1, our_malloc(size), SUCCESS, false);
    check_result(size + 1, 1, our_malloc(size + 1), SUCCESS, false);
  }

  test_calloc(0, 0, SUCCESS, false);

  for (sizeclass_t sc = 0; sc < NUM_SIZECLASSES; sc++)
  {
    const size_t size = sizeclass_to_size(sc);

    bool overflow = false;
    for (size_t n = 1; bits::umul(size, n, overflow) <= SUPERSLAB_SIZE; n *= 5)
    {
      if (overflow)
        break;

      test_calloc(n, size, SUCCESS, false);
      test_calloc(n, 0, SUCCESS, false);
    }
    test_calloc(0, size, SUCCESS, false);
  }

  for (sizeclass_t sc = 0; sc < NUM_SIZECLASSES; sc++)
  {
    const size_t size = sizeclass_to_size(sc);
    test_realloc(our_malloc(size), size, SUCCESS, false);
    test_realloc(our_malloc(size), 0, SUCCESS, true);
    test_realloc(nullptr, size, SUCCESS, false);
    test_realloc(our_malloc(size), (size_t)-1, ENOMEM, true);
    for (sizeclass_t sc2 = 0; sc2 < NUM_SIZECLASSES; sc2++)
    {
      const size_t size2 = sizeclass_to_size(sc2);
      test_realloc(our_malloc(size), size2, SUCCESS, false);
      test_realloc(our_malloc(size + 1), size2, SUCCESS, false);
    }
  }

  for (sizeclass_t sc = 0; sc < (SUPERSLAB_BITS + 4); sc++)
  {
    const size_t size = 1ULL << sc;
    test_realloc(our_malloc(size), size, SUCCESS, false);
    test_realloc(our_malloc(size), 0, SUCCESS, true);
    test_realloc(nullptr, size, SUCCESS, false);
    test_realloc(our_malloc(size), (size_t)-1, ENOMEM, true);
    for (sizeclass_t sc2 = 0; sc2 < (SUPERSLAB_BITS + 4); sc2++)
    {
      const size_t size2 = 1ULL << sc2;
      printf("size1: %zu, size2:%zu\n", size, size2);
      test_realloc(our_malloc(size), size2, SUCCESS, false);
      test_realloc(our_malloc(size + 1), size2, SUCCESS, false);
    }
  }

  test_posix_memalign(0, 0, EINVAL, true);
  test_posix_memalign((size_t)-1, 0, EINVAL, true);
  test_posix_memalign(OS_PAGE_SIZE, sizeof(uintptr_t) / 2, EINVAL, true);

  for (size_t align = sizeof(uintptr_t); align <= SUPERSLAB_SIZE * 8;
       align <<= 1)
  {
    for (sizeclass_t sc = 0; sc < NUM_SIZECLASSES; sc++)
    {
      const size_t size = sizeclass_to_size(sc);
      test_posix_memalign(size, align, SUCCESS, false);
      test_posix_memalign(size, 0, EINVAL, true);
      test_memalign(size, align, SUCCESS, false);
    }
    test_posix_memalign(0, align, SUCCESS, false);
    test_posix_memalign((size_t)-1, align, ENOMEM, true);
    test_posix_memalign(0, align + 1, EINVAL, true);
  }

  current_alloc_pool()->debug_check_empty();
  return 0;
}
