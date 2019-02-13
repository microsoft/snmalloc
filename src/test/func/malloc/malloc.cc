#include <cassert>
#include <cerrno>
#include <malloc.h>
#include <snmalloc.h>

using namespace snmalloc;

void check_result(size_t size, void* p, int err, bool null)
{
  assert(errno == err);
  UNUSED(err);
  if (null)
  {
    assert(p == nullptr);
  }
  else
  {
    assert(malloc_usable_size(p) >= size);
    UNUSED(size);
    free(p);
  }
}

void test_calloc(size_t nmemb, size_t size, int err, bool null)
{
  fprintf(stderr, "calloc(%d, %d)\n", (int)nmemb, (int)size);
  errno = 0;
  void* p = calloc(nmemb, size);

  if ((p != nullptr) && (errno == 0))
    for (size_t i = 0; i < (size * nmemb); i++)
      assert(((uint8_t*)p)[i] == 0);

  check_result(nmemb * size, p, err, null);
}

void test_realloc(void* p, size_t size, int err, bool null)
{
  fprintf(stderr, "realloc(%p (%d), %d)\n", p, int(size), (int)size);
  errno = 0;
  p = realloc(p, size);
  check_result(size, p, err, null);
}

void test_posix_memalign(size_t size, size_t align, int err, bool null)
{
  fprintf(stderr, "posix_memalign(&p, %d, %d)\n", (int)align, (int)size);
  void* p = nullptr;
  errno = posix_memalign(&p, align, size);
  check_result(size, p, err, null);
  if (!null)
    assert(((uintptr_t)p % align) == 0);
}

void test_memalign(size_t size, size_t align, int err, bool null)
{
  fprintf(stderr, "memalign(%d, %d)\n", (int)align, (int)size);
  errno = 0;
  void* p = memalign(align, size);
  check_result(size, p, err, null);
  if (!null)
    assert(((uintptr_t)p % align) == 0);
}

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);

  constexpr int SUCCESS = 0;

  for (uint8_t sc = 0; sc < NUM_SIZECLASSES; sc++)
  {
    const size_t size = sizeclass_to_size(sc);

    test_calloc(0, 0, SUCCESS, false);
    test_calloc((size_t)-1, 1, ENOMEM, true);

    bool overflow = false;
    for (size_t n = 1; bits::umul(size, n, overflow) < SUPERSLAB_SIZE; n *= 5)
    {
      if (overflow)
        break;

      test_calloc(n, size, SUCCESS, false);
    }

    test_realloc(malloc(size), 0, SUCCESS, true);
    test_realloc(nullptr, size, SUCCESS, false);
    test_realloc(malloc(size), (size_t)-1, ENOMEM, true);
  }

  test_posix_memalign(0, 0, EINVAL, true);
  test_posix_memalign((size_t)-1, 0, EINVAL, true);

  for (size_t align = sizeof(size_t); align <= SUPERSLAB_SIZE; align <<= 1)
  {
    for (uint8_t sc = 0; sc < NUM_SIZECLASSES; sc++)
    {
      const size_t size = sizeclass_to_size(sc);
      test_posix_memalign(size, align, SUCCESS, false);
      test_memalign(size, align, SUCCESS, false);
    }
    test_posix_memalign(0, align, SUCCESS, false);
    test_posix_memalign((size_t)-1, align, ENOMEM, true);
    test_posix_memalign(0, align + 1, EINVAL, true);
  }

  return 0;
}
