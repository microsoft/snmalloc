#include <ios>
#include <iostream>
#include <iomanip>
#include <test/setup.h>

#define SNMALLOC_NAME_MANGLE(a) our_##a
#undef SNMALLOC_NO_REALLOCARRAY
#undef SNMALLOC_NO_REALLOCARR
#include "../../../override/malloc.cc"

using namespace snmalloc;

constexpr int SUCCESS = 0;

void check_result(size_t size, size_t align, void* p, int err, bool null)
{
  bool failed = false;
  if (errno != err && err != SUCCESS)
  {
    std::cout << "Expected error: " << err << " but got " << errno << std::endl;
    failed = true;
  }

  if (null)
  {
    if (p != nullptr)
    {
      std::cout << "Expected null, and got non-null return!" << std::endl;
      abort();
    }
    return;
  }

  if ((p == nullptr) && (size != 0))
  {
    std::cout << "Unexpected null returned." << std::endl;
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
    std::cout << "Usable size is " << alloc_size << ", but required to be " << expected_size << "." << std::endl;
    failed = true;
  }
  if ((!exact_size) && (alloc_size < expected_size))
  {
    std::cout << "Usable size is " << alloc_size << ", but required to be at least " << expected_size << "." << std::endl;
    failed = true;
  }
  if (
    (static_cast<size_t>(reinterpret_cast<uintptr_t>(p) % align) != 0) &&
    (size != 0))
  {
    std::ios_base::fmtflags f(std::cout.flags());
    std::cout << std::hex << "Address is " << p << ", but required to be aligned to 0x" << align << "." << std::endl;
    std::cout.flags(f);
    failed = true;
  }
  if (
    static_cast<size_t>(
      reinterpret_cast<uintptr_t>(p) % natural_alignment(size)) != 0)
  {
    std::ios_base::fmtflags f(std::cout.flags());
    std::cout << std::hex << "Address is " << p << ", but should have natural alignment to 0x" << natural_alignment(size) << "." << std::endl;
    std::cout.flags(f);
    failed = true;
  }

  if (failed)
  {
    std::cout << "check_result failed! " << p << std::endl;
    abort();
  }
  our_free(p);
}

void test_calloc(size_t nmemb, size_t size, int err, bool null)
{
  std::cout << "calloc(" << nmemb << ", " << size <<  ")" << " combined size " << nmemb * size << std::endl;
  errno = SUCCESS;
  void* p = our_calloc(nmemb, size);

  if (p != nullptr)
  {
    for (size_t i = 0; i < (size * nmemb); i++)
    {
      if (((uint8_t*)p)[i] != 0)
      {
        std::cout << "non-zero at @" << i << std::endl;
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

  std::cout << "realloc(" << p << "(" << old_size << ")" << ", " << size << ")" << std::endl;
  errno = SUCCESS;
  auto new_p = our_realloc(p, size);
  // Realloc failure case, deallocate original block
  if (new_p == nullptr && size != 0)
    our_free(p);
  check_result(size, 1, new_p, err, null);
}

void test_posix_memalign(size_t size, size_t align, int err, bool null)
{
  std::cout << "posix_memalign(&p, " << align << ", " << size << ")" << std::endl;
  void* p = nullptr;
  errno = our_posix_memalign(&p, align, size);
  check_result(size, align, p, err, null);
}

void test_memalign(size_t size, size_t align, int err, bool null)
{
  std::cout << "memalign(" << align << ", " << size << ")" << std::endl;
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

  std::cout << "reallocarray(" << p << "(" << old_size << ")" << ", " << tsize << ")" << std::endl;
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
  errno = SUCCESS;
  int r = our_reallocarr(&p, nmemb, size);
  if (r != err)
  {
    std::cout << "reallocarr failed! expected " << err << " got " << r << std::endl;
    abort();
  }

  std::cout << "reallocarr(" << p << "(" << nmemb << ")" << ", " << size << ")" << std::endl;
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
    if (static_cast<char*>(p)[i] != 1)
    {
      std::cout << "data consistency failed! at " << i << std::endl;
      abort();
    }
  }
  our_free(p);
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
    std::cout << "malloc: " << size << std::endl;
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
      std::cout << "size1: " << size << ", size2: " << size2 << std::endl;
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

  test_reallocarray(nullptr, 1, 0, SUCCESS, false);
  for (smallsizeclass_t sc = 0; sc < (MAX_SMALL_SIZECLASS_BITS + 4); sc++)
  {
    const size_t size = bits::one_at_bit(sc);
    test_reallocarray(our_malloc(size), 1, size, SUCCESS, false);
    test_reallocarray(our_malloc(size), 1, 0, SUCCESS, false);
    test_reallocarray(nullptr, 1, size, SUCCESS, false);
    test_reallocarray(our_malloc(size), 1, ((size_t)-1) / 2, ENOMEM, true);
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
    if (p == nullptr)
    {
      std::cout << "realloc alloc failed with " << size << std::endl;
      abort();
    }
    int r = our_reallocarr(&p, 1, ((size_t)-1) / 2);
    if (r != ENOMEM)
    {
      std::cout << "expected failure on allocation" << std::endl;
      abort();
    }
    our_free(p);

    for (smallsizeclass_t sc2 = 0; sc2 < (MAX_SMALL_SIZECLASS_BITS + 4); sc2++)
    {
      const size_t size2 = bits::one_at_bit(sc2);
      std::cout << "size1: " << size << ", size2: " << size2 << std::endl;
      test_reallocarr(size, 1, size2, SUCCESS, false);
    }
  }

  if (our_malloc_usable_size(nullptr) != 0)
  {
    std::cout << "malloc_usable_size(nullptr) should be zero" << std::endl;
    abort();
  }

  snmalloc::debug_check_empty<snmalloc::Globals>();
  return 0;
}
