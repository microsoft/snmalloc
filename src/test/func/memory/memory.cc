#include <array>
#include <chrono>
#include <iostream>
#include <snmalloc/snmalloc.h>
#include <test/opt.h>
#include <test/setup.h>
#include <test/xoroshiro.h>
#include <unordered_set>
#include <vector>

#if ((defined(__linux__) && !defined(__ANDROID__)) || defined(__sun)) || \
  defined(__OpenBSD__) && !defined(SNMALLOC_QEMU_WORKAROUND)
/*
 * We only test allocations with limited AS on linux and Solaris for now.
 * It should be a good representative for POSIX systems.
 * QEMU `setrlimit64` does not behave as the same as native linux,
 * so we need to exclude it from such tests.
 */
#  include <sys/resource.h>
#  ifndef __OpenBSD__
#    include <sys/sysinfo.h>
#  endif
#  include <sys/wait.h>
#  include <unistd.h>
#  define TEST_LIMITED
#  define KiB (1024ull)
#  define MiB (KiB * KiB)
#  define GiB (KiB * MiB)

#  ifdef __OpenBSD__
using rlim64_t = rlim_t;
#  endif
#else
using rlim64_t = size_t;
#endif

using namespace snmalloc;

void test_limited(rlim64_t as_limit, size_t& count)
{
  UNUSED(as_limit, count);
#if false && defined(TEST_LIMITED)
  auto pid = fork();
  if (!pid)
  {
    auto limit = rlimit64{.rlim_cur = as_limit, .rlim_max = RLIM64_INFINITY};
    if (setrlimit64(RLIMIT_AS, &limit))
    {
      std::abort();
    }
    if (getrlimit64(RLIMIT_AS, &limit))
    {
      std::abort();
    }
    std::cout << "limiting memory to " << limit.rlim_cur / KiB << " KiB"
              << std::endl;
#  ifndef __OpenBSD__
    struct sysinfo info
    {};
    if (sysinfo(&info))
    {
      std::abort();
    }
    std::cout << "host freeram: " << info.freeram / KiB << " KiB" << std::endl;
    // set the allocation size to the minimum value among:
    // 2GiB, 1/8 of the AS limit, 1/8 of the Free RAM
    auto upper_bound =
      std::min(static_cast<unsigned long long>(limit.rlim_cur >> 3u), 2 * GiB);
    upper_bound = std::min(
      upper_bound, static_cast<unsigned long long>(info.freeram >> 3u));
    std::cout << "trying to alloc " << upper_bound / KiB << " KiB" << std::endl;
#  endif
    std::cout << "allocator initialised" << std::endl;
    auto chunk = snmalloc::alloc(upper_bound);
    snmalloc::dealloc(chunk);
    std::cout << "success" << std::endl;
    std::exit(0);
  }
  else
  {
    int status;
    waitpid(pid, &status, 0);
    if (status)
    {
      std::cout << "failed" << std::endl;
      count++;
    }
  }
#endif
}

void test_alloc_dealloc_64k()
{
  constexpr size_t count = 1 << 12;
  constexpr size_t outer_count = 12;
  void* garbage[count];
  void* keep_alive[outer_count];

  for (size_t j = 0; j < outer_count; j++)
  {
    // Allocate 64k of 16byte allocs
    // This will fill the short slab, and then start a new slab.
    for (size_t i = 0; i < count; i++)
    {
      garbage[i] = snmalloc::alloc(16);
    }

    // Allocate one object on the second slab
    keep_alive[j] = snmalloc::alloc(16);

    for (size_t i = 0; i < count; i++)
    {
      snmalloc::dealloc(garbage[i]);
    }
  }
  for (size_t j = 0; j < outer_count; j++)
  {
    snmalloc::dealloc(keep_alive[j]);
  }
}

void test_random_allocation()
{
  std::unordered_set<void*> allocated;

  constexpr size_t count = 10000;
  constexpr size_t outer_count = 10;
  void* objects[count];
  for (size_t i = 0; i < count; i++)
    objects[i] = nullptr;

  // Randomly allocate and deallocate objects
  xoroshiro::p128r32 r;
  size_t alloc_count = 0;
  for (size_t j = 0; j < outer_count; j++)
  {
    auto just_dealloc = r.next() % 2 == 1;
    auto duration = r.next() % count;
    for (size_t i = 0; i < duration; i++)
    {
      auto index = r.next();
      auto& cell = objects[index % count];
      if (cell != nullptr)
      {
        allocated.erase(cell);
        snmalloc::dealloc(cell);
        cell = nullptr;
        alloc_count--;
      }
      if (!just_dealloc)
      {
        cell = snmalloc::alloc(16);
        auto pair = allocated.insert(cell);
        // Check not already allocated
        SNMALLOC_CHECK(pair.second);
        UNUSED(pair);
        alloc_count++;
      }
      else
      {
        if (alloc_count == 0 && just_dealloc)
          break;
      }
    }
  }

  // Deallocate all the remaining objects
  for (size_t i = 0; i < count; i++)
    if (objects[i] != nullptr)
      snmalloc::dealloc(objects[i]);
}

void test_calloc()
{
  for (size_t size = 16; size <= (1 << 24); size <<= 1)
  {
    void* p = snmalloc::alloc(size);
    memset(p, 0xFF, size);
    snmalloc::dealloc(p, size);

    p = snmalloc::alloc<Zero>(size);

    for (size_t i = 0; i < size; i++)
    {
      if (((char*)p)[i] != 0)
        abort();
    }

    snmalloc::dealloc(p, size);
  }

  snmalloc::debug_check_empty();
}

void test_double_alloc()
{
  {
    auto a1 = snmalloc::get_scoped_allocator();
    auto a2 = snmalloc::get_scoped_allocator();

    const size_t n = (1 << 16) / 32;

    for (size_t k = 0; k < 4; k++)
    {
      std::unordered_set<void*> set1;
      std::unordered_set<void*> set2;

      for (size_t i = 0; i < (n * 2); i++)
      {
        void* p = a1->alloc(20);
        SNMALLOC_CHECK(set1.find(p) == set1.end());
        set1.insert(p);
      }

      for (size_t i = 0; i < (n * 2); i++)
      {
        void* p = a2->alloc(20);
        SNMALLOC_CHECK(set2.find(p) == set2.end());
        set2.insert(p);
      }

      while (!set1.empty())
      {
        auto it = set1.begin();
        a2->dealloc(*it);
        set1.erase(it);
      }

      while (!set2.empty())
      {
        auto it = set2.begin();
        a1->dealloc(*it);
        set2.erase(it);
      }
    }
  }
  snmalloc::debug_check_empty();
}

void test_external_pointer()
{
  for (snmalloc::smallsizeclass_t sc = size_to_sizeclass(MIN_ALLOC_SIZE);
       sc < NUM_SMALL_SIZECLASSES;
       sc++)
  {
    size_t size = sizeclass_to_size(sc);
    void* p1 = snmalloc::alloc(size);

    if (size != snmalloc::alloc_size(p1))
    {
      if (size > snmalloc::alloc_size(p1) || snmalloc::is_owned(p1))
      {
        std::cout << "Requested size: " << size
                  << " alloc_size: " << snmalloc::alloc_size(p1) << std::endl;
        abort();
      }
    }

    for (size_t offset = 0; offset < size; offset += 17)
    {
      void* p2 = pointer_offset(p1, offset);
      void* p3 = snmalloc::external_pointer(p2);
      void* p4 = snmalloc::external_pointer<End>(p2);
      if (p1 != p3)
      {
        if (p3 > p1 || snmalloc::is_owned(p1))
        {
          std::cout << "size: " << size
                    << " alloc_size: " << snmalloc::alloc_size(p1)
                    << " offset: " << offset << " p1: " << p1 << "  p3: " << p3
                    << std::endl;
          abort();
        }
      }

      if ((size_t)p4 != (size_t)p1 + size - 1)
      {
        if (((size_t)p4 < (size_t)p1 + size - 1) || snmalloc::is_owned(p1))
        {
          std::cout << "size: " << size << " end(p4): " << p4 << " p1: " << p1
                    << "  p1+size-1: " << pointer_offset(p1, size - 1)
                    << std::endl;
          abort();
        }
      }
    }

    snmalloc::dealloc(p1, size);
  }

  snmalloc::debug_check_empty();
};

void check_offset(void* base, void* interior)
{
  void* calced_base = snmalloc::external_pointer((void*)interior);
  if (calced_base != (void*)base)
  {
    if (calced_base > base || snmalloc::is_owned(base))
    {
      std::cout << "Calced base: " << calced_base << " actual base: " << base
                << " for interior: " << interior << std::endl;
      abort();
    }
  }
}

void check_external_pointer_large(size_t* base)
{
  size_t size = *base;
  char* curr = (char*)base;
  for (size_t offset = 0; offset < size; offset += 1 << 24)
  {
    check_offset(base, (void*)(curr + offset));
    check_offset(base, (void*)(curr + offset + (1 << 24) - 1));
  }
}

void test_external_pointer_large()
{
  xoroshiro::p128r64 r;

  constexpr size_t count_log = DefaultPal::address_bits > 32 ? 5 : 3;
  constexpr size_t count = 1 << count_log;
  // Pre allocate all the objects
  size_t* objects[count];

  size_t total_size = 0;

  for (size_t i = 0; i < count; i++)
  {
    size_t b = MAX_SMALL_SIZECLASS_BITS + 3;
    size_t rand = r.next() & ((1 << b) - 1);
    size_t size = (1 << 24) + rand;
    total_size += size;
    // store object
    objects[i] = (size_t*)snmalloc::alloc(size);
    // Store allocators size for this object
    *objects[i] = snmalloc::alloc_size(objects[i]);

    check_external_pointer_large(objects[i]);
    if (i > 0)
      check_external_pointer_large(objects[i - 1]);
  }

  for (size_t i = 0; i < count; i++)
  {
    check_external_pointer_large(objects[i]);
  }

  std::cout << "Total size allocated in test_external_pointer_large: "
            << total_size << std::endl;

  // Deallocate everything
  for (size_t i = 0; i < count; i++)
  {
    snmalloc::dealloc(objects[i]);
  }
}

void test_external_pointer_dealloc_bug()
{
  std::cout << "Testing external pointer dealloc bug" << std::endl;
  constexpr size_t count = MIN_CHUNK_SIZE;
  void* allocs[count];

  for (size_t i = 0; i < count; i++)
  {
    allocs[i] = snmalloc::alloc(MIN_CHUNK_BITS / 2);
  }

  for (size_t i = 1; i < count; i++)
  {
    snmalloc::dealloc(allocs[i]);
  }

  for (size_t i = 0; i < count; i++)
  {
    snmalloc::external_pointer(allocs[i]);
  }

  snmalloc::dealloc(allocs[0]);
  std::cout << "Testing external pointer dealloc bug - done" << std::endl;
}

void test_external_pointer_stack()
{
  std::cout << "Testing external pointer stack" << std::endl;

  std::array<int, 2000> stack;

  for (size_t i = 0; i < stack.size(); i++)
  {
    if (snmalloc::external_pointer(&stack[i]) > &stack[i])
    {
      std::cout << "Stack pointer: " << &stack[i] << " external pointer: "
                << snmalloc::external_pointer(&stack[i]) << std::endl;
      abort();
    }
  }

  std::cout << "Testing external pointer stack - done" << std::endl;
}

void test_alloc_16M()
{
  // sizes >= 16M use large_alloc
  const size_t size = 16'000'000;

  void* p1 = snmalloc::alloc(size);
  SNMALLOC_CHECK(snmalloc::alloc_size(snmalloc::external_pointer(p1)) >= size);
  snmalloc::dealloc(p1);
}

void test_calloc_16M()
{
  // sizes >= 16M use large_alloc
  const size_t size = 16'000'000;

  void* p1 = snmalloc::alloc<Zero>(size);
  SNMALLOC_CHECK(snmalloc::alloc_size(snmalloc::external_pointer(p1)) >= size);
  snmalloc::dealloc(p1);
}

void test_calloc_large_bug()
{
  // Perform large calloc, to check for correct zeroing from PAL.
  // Some PALS have special paths for PAGE aligned zeroing of large
  // allocations.  This is a large allocation that is intentionally
  // not a multiple of page size.
  const size_t size = (MAX_SMALL_SIZECLASS_SIZE << 3) - 7;

  void* p1 = snmalloc::alloc<Zero>(size);
  SNMALLOC_CHECK(snmalloc::alloc_size(snmalloc::external_pointer(p1)) >= size);
  snmalloc::dealloc(p1);
}

template<size_t asz, int dealloc = 2>
void test_static_sized_alloc()
{
  auto p = snmalloc::alloc<asz>();

  static_assert((dealloc >= 0) && (dealloc <= 2), "bad dealloc flavor");
  switch (dealloc)
  {
    case 0:
      snmalloc::dealloc(p);
      break;
    case 1:
      snmalloc::dealloc(p, asz);
      break;
    case 2:
      snmalloc::dealloc<asz>(p);
      break;
  }

  if constexpr (dealloc != 0)
    test_static_sized_alloc<asz, dealloc - 1>();
}

template<size_t max_size = bits::one_at_bit(23)>
void test_static_sized_allocs()
{
  if (max_size < 16)
    return;

  constexpr size_t next_size = max_size >> 1;
  test_static_sized_allocs<next_size>();

  test_static_sized_alloc<max_size * 3>();
  test_static_sized_alloc<max_size * 5>();
  test_static_sized_alloc<max_size * 7>();
  test_static_sized_alloc<max_size * 1>();

  test_static_sized_alloc<max_size * 3 - 1>();
  test_static_sized_alloc<max_size * 5 - 1>();
  test_static_sized_alloc<max_size * 7 - 1>();
  test_static_sized_alloc<max_size * 1 - 1>();

  test_static_sized_alloc<max_size * 3 + 1>();
  test_static_sized_alloc<max_size * 5 + 1>();
  test_static_sized_alloc<max_size * 7 + 1>();
  test_static_sized_alloc<max_size * 1 + 1>();
}

void test_remaining_bytes()
{
  for (snmalloc::smallsizeclass_t sc = size_to_sizeclass(MIN_ALLOC_SIZE);
       sc < NUM_SMALL_SIZECLASSES;
       sc++)
  {
    auto size = sizeclass_to_size(sc);
    char* p = (char*)snmalloc::alloc(size);
    for (size_t offset = 0; offset < size; offset++)
    {
      auto rem =
        snmalloc::remaining_bytes(address_cast(pointer_offset(p, offset)));
      if (rem != (size - offset))
      {
        if (rem < (size - offset) || snmalloc::is_owned(p))
        {
          report_fatal_error(
            "Allocation size: {},  Offset: {},  Remaining bytes: {}, "
            "Expected: {}",
            size,
            offset,
            rem,
            size - offset);
        }
      }
    }
    snmalloc::dealloc(p);
  }
}

void test_consolidaton_bug()
{
  /**
   * Check for consolidation of various sizes, but allocating and deallocating,
   * then requesting larger sizes. See issue #506
   */
  for (size_t i = 0; i < 27; i++)
  {
    std::vector<void*> allocs;
    for (size_t j = 0; j < 4; j++)
    {
      allocs.push_back(snmalloc::alloc(bits::one_at_bit(i)));
    }
    for (auto a : allocs)
    {
      snmalloc::dealloc(a);
    }
  }
}

int main(int, char**)
{
  setup();
#ifdef TEST_LIMITED
  size_t count = 0;
  test_limited(512 * MiB, count);
  test_limited(2 * GiB, count);
  test_limited(
    8 *
      GiB, // 8 * GiB is large enough for a loose upper-bound of our allocations
    count);
  if (count)
  {
    std::cout << count << " attempts failed out of 3" << std::endl;
    std::abort();
  }
#endif
  auto start = std::chrono::steady_clock::now();
#define TEST(testname) \
  do \
  { \
    auto end = std::chrono::steady_clock::now(); \
    auto diff_seconds = \
      std::chrono::duration_cast<std::chrono::seconds>(end - start).count(); \
    std::cout << "Running " #testname << " @ " << diff_seconds << std::endl; \
    for (size_t i = 0; i < 50; i++) \
      testname(); \
  } while (0);

  TEST(test_alloc_dealloc_64k);
  TEST(test_random_allocation);
  TEST(test_calloc);
  TEST(test_double_alloc);
  TEST(test_remaining_bytes);
  TEST(test_static_sized_allocs);
  TEST(test_calloc_large_bug);
  TEST(test_external_pointer_stack);
  TEST(test_external_pointer_dealloc_bug);
  TEST(test_external_pointer_large);
  TEST(test_external_pointer);
  TEST(test_alloc_16M);
  TEST(test_calloc_16M);
  TEST(test_consolidaton_bug);

  std::cout << "Tests completeed successfully!" << std::endl;
  return 0;
}
