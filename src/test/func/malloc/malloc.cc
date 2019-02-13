#include <cassert>
#include <cerrno>
#include <ctime>
#include <malloc.h>
#include <snmalloc.h>
#include <test/xoroshiro.h>

using namespace snmalloc;

constexpr size_t min_pow_2 = bits::ctz_const(sizeof(size_t));
constexpr size_t max_pow_2 = bits::ctz_const(SUPERSLAB_SIZE);

size_t rand_pow_2(xoroshiro::p128r64& r, size_t min_pow = min_pow_2)
{
  const size_t max_pow = max_pow_2 - min_pow;
  auto p = (r.next() % (max_pow + 1)) + min_pow;
  return (size_t)1 << p;
}

size_t rand_non_pow_2(xoroshiro::p128r64& r, size_t min = 0)
{
  const size_t max = (1 << max_pow_2) - min;
  auto n = (r.next() % (max + 1)) + min;
  while (__builtin_popcount(n) < 2)
    n = r.next();

  return n;
}

void check_result(size_t size, void* p, int err, bool null)
{
  fprintf(stderr, "%p, errno: %s\n", p, strerror(errno)); // TODO
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
  fprintf(stderr, "realloc(%p, %d)\n", p, (int)size);
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

  auto r = xoroshiro::p128r64((size_t)time(nullptr));

  size_t n, s;
  do
  {
    n = rand_pow_2(r);
    s = rand_pow_2(r);
  } while ((n * s) > (1 << max_pow_2));
  test_calloc(n, s, SUCCESS, false);
  test_calloc(0, 0, SUCCESS, false);
  test_calloc((size_t)-1, 1, ENOMEM, true);

  test_realloc(malloc(rand_pow_2(r)), 0, SUCCESS, true);
  test_realloc(nullptr, rand_pow_2(r), SUCCESS, false);
  test_realloc(malloc(rand_pow_2(r)), (size_t)-1, ENOMEM, true);

  test_posix_memalign(rand_pow_2(r), rand_pow_2(r), SUCCESS, false);
  test_posix_memalign(0, 0, EINVAL, true);
  test_posix_memalign(0, rand_pow_2(r), SUCCESS, false);
  test_posix_memalign((size_t)-1, 0, EINVAL, true);
  test_posix_memalign(0, rand_non_pow_2(r), EINVAL, true);
  test_posix_memalign((size_t)-1, rand_pow_2(r), ENOMEM, true);

  test_memalign(rand_pow_2(r), rand_pow_2(r), SUCCESS, false);

  // snmalloc and glibc pass, not jemalloc
  test_memalign((size_t)-1, rand_pow_2(r), ENOMEM, true);

  // glibc passes, not snmalloc or jemalloc
  // test_memalign((size_t)-1, 0, ENOMEM, true);

  return 0;
}
