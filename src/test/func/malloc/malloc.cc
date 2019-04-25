#include <stdio.h>

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
  }
  else
  {
    if (our_malloc_usable_size(p) < size)
      abort();

    if (((uintptr_t)p % align) != 0)
      abort();

    our_free(p);
  }
}

void test_calloc(size_t nmemb, size_t size, int err, bool null)
{
  fprintf(stderr, "calloc(%d, %d)\n", (int)nmemb, (int)size);
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
  fprintf(stderr, "realloc(%p(%d), %d)\n", p, int(size), (int)size);
  errno = 0;
  p = our_realloc(p, size);
  check_result(size, 1, p, err, null);
}

void test_posix_memalign(size_t size, size_t align, int err, bool null)
{
  fprintf(stderr, "posix_memalign(&p, %d, %d)\n", (int)align, (int)size);
  void* p = nullptr;
  errno = our_posix_memalign(&p, align, size);
  check_result(size, align, p, err, null);
}

void test_memalign(size_t size, size_t align, int err, bool null)
{
  fprintf(stderr, "memalign(%d, %d)\n", (int)align, (int)size);
  errno = 0;
  void* p = our_memalign(align, size);
  check_result(size, align, p, err, null);
}

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);

  constexpr int SUCCESS = 0;

  test_calloc(0, 0, SUCCESS, false);

  for (uint8_t sc = 0; sc < NUM_SIZECLASSES; sc++)
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

    test_realloc(our_malloc(size), size, SUCCESS, false);
    test_realloc(our_malloc(size), 0, SUCCESS, true);
    test_realloc(nullptr, size, SUCCESS, false);
    test_realloc(our_malloc(size), (size_t)-1, ENOMEM, true);
  }

  test_posix_memalign(0, 0, EINVAL, true);
  test_posix_memalign((size_t)-1, 0, EINVAL, true);

  for (size_t align = sizeof(size_t); align <= SUPERSLAB_SIZE; align <<= 1)
  {
    for (uint8_t sc = 0; sc < NUM_SIZECLASSES; sc++)
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

  return 0;
}
