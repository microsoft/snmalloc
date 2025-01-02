#include <array>
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
    auto& alloc = ThreadAlloc::get();
    std::cout << "allocator initialised" << std::endl;
    auto chunk = alloc.alloc(upper_bound);
    alloc.dealloc(chunk);
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
  auto& alloc = ThreadAlloc::get();

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
      garbage[i] = alloc.alloc(16);
    }

    // Allocate one object on the second slab
    keep_alive[j] = alloc.alloc(16);

    for (size_t i = 0; i < count; i++)
    {
      alloc.dealloc(garbage[i]);
    }
  }
  for (size_t j = 0; j < outer_count; j++)
  {
    alloc.dealloc(keep_alive[j]);
  }
}

void test_random_allocation()
{
  auto& alloc = ThreadAlloc::get();
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
        alloc.dealloc(cell);
        cell = nullptr;
        alloc_count--;
      }
      if (!just_dealloc)
      {
        cell = alloc.alloc(16);
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
      alloc.dealloc(objects[i]);
}

void test_calloc()
{
  auto& alloc = ThreadAlloc::get();

  for (size_t size = 16; size <= (1 << 24); size <<= 1)
  {
    void* p = alloc.alloc(size);
    memset(p, 0xFF, size);
    alloc.dealloc(p, size);

    p = alloc.alloc<YesZero>(size);

    for (size_t i = 0; i < size; i++)
    {
      if (((char*)p)[i] != 0)
        abort();
    }

    alloc.dealloc(p, size);
  }

  snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
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
        a2->dealloc(*it, 20);
        set1.erase(it);
      }

      while (!set2.empty())
      {
        auto it = set2.begin();
        a1->dealloc(*it, 20);
        set2.erase(it);
      }
    }
  }
  snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
}

void test_external_pointer()
{
  // Malloc does not have an external pointer querying mechanism.
  auto& alloc = ThreadAlloc::get();

  for (snmalloc::smallsizeclass_t sc = size_to_sizeclass(MIN_ALLOC_SIZE);
       sc < NUM_SMALL_SIZECLASSES;
       sc++)
  {
    size_t size = sizeclass_to_size(sc);
    void* p1 = alloc.alloc(size);

    if (size != alloc.alloc_size(p1))
    {
      std::cout << "Requested size: " << size
                << " alloc_size: " << alloc.alloc_size(p1) << std::endl;
      abort();
    }

    for (size_t offset = 0; offset < size; offset += 17)
    {
      void* p2 = pointer_offset(p1, offset);
      void* p3 = alloc.external_pointer(p2);
      void* p4 = alloc.external_pointer<End>(p2);
      if (p1 != p3)
      {
        std::cout << "size: " << size << " alloc_size: " << alloc.alloc_size(p1)
                  << " offset: " << offset << " p1: " << p1 << "  p3: " << p3
                  << std::endl;
      }
      SNMALLOC_CHECK(p1 == p3);
      if ((size_t)p4 != (size_t)p1 + size - 1)
      {
        std::cout << "size: " << size << " end(p4): " << p4 << " p1: " << p1
                  << "  p1+size-1: " << pointer_offset(p1, size - 1)
                  << std::endl;
      }
      SNMALLOC_CHECK((size_t)p4 == (size_t)p1 + size - 1);
    }

    alloc.dealloc(p1, size);
  }

  snmalloc::debug_check_empty<snmalloc::Alloc::Config>();
};

void check_offset(void* base, void* interior)
{
  auto& alloc = ThreadAlloc::get();
  void* calced_base = alloc.external_pointer((void*)interior);
  if (calced_base != (void*)base)
  {
    std::cout << "Calced base: " << calced_base << " actual base: " << base
              << " for interior: " << interior << std::endl;
    abort();
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

  auto& alloc = ThreadAlloc::get();

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
    objects[i] = (size_t*)alloc.alloc(size);
    // Store allocators size for this object
    *objects[i] = alloc.alloc_size(objects[i]);

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
    alloc.dealloc(objects[i]);
  }
}

void test_external_pointer_dealloc_bug()
{
  std::cout << "Testing external pointer dealloc bug" << std::endl;
  auto& alloc = ThreadAlloc::get();
  constexpr size_t count = MIN_CHUNK_SIZE;
  void* allocs[count];

  for (size_t i = 0; i < count; i++)
  {
    allocs[i] = alloc.alloc(MIN_CHUNK_BITS / 2);
  }

  for (size_t i = 1; i < count; i++)
  {
    alloc.dealloc(allocs[i]);
  }

  for (size_t i = 0; i < count; i++)
  {
    alloc.external_pointer(allocs[i]);
  }

  alloc.dealloc(allocs[0]);
  std::cout << "Testing external pointer dealloc bug - done" << std::endl;
}

void test_external_pointer_stack()
{
  std::cout << "Testing external pointer stack" << std::endl;

  std::array<int, 2000> stack;

  auto& alloc = ThreadAlloc::get();

  for (size_t i = 0; i < stack.size(); i++)
  {
    if (alloc.external_pointer(&stack[i]) > &stack[i])
    {
      std::cout << "Stack pointer: " << &stack[i]
                << " external pointer: " << alloc.external_pointer(&stack[i])
                << std::endl;
      abort();
    }
  }

  std::cout << "Testing external pointer stack - done" << std::endl;
}

void test_alloc_16M()
{
  auto& alloc = ThreadAlloc::get();
  // sizes >= 16M use large_alloc
  const size_t size = 16'000'000;

  void* p1 = alloc.alloc(size);
  SNMALLOC_CHECK(alloc.alloc_size(alloc.external_pointer(p1)) >= size);
  alloc.dealloc(p1);
}

void test_calloc_16M()
{
  auto& alloc = ThreadAlloc::get();
  // sizes >= 16M use large_alloc
  const size_t size = 16'000'000;

  void* p1 = alloc.alloc<YesZero>(size);
  SNMALLOC_CHECK(alloc.alloc_size(alloc.external_pointer(p1)) >= size);
  alloc.dealloc(p1);
}

void test_calloc_large_bug()
{
  auto& alloc = ThreadAlloc::get();
  // Perform large calloc, to check for correct zeroing from PAL.
  // Some PALS have special paths for PAGE aligned zeroing of large
  // allocations.  This is a large allocation that is intentionally
  // not a multiple of page size.
  const size_t size = (MAX_SMALL_SIZECLASS_SIZE << 3) - 7;

  void* p1 = alloc.alloc<YesZero>(size);
  SNMALLOC_CHECK(alloc.alloc_size(alloc.external_pointer(p1)) >= size);
  alloc.dealloc(p1);
}

template<size_t asz, int dealloc>
void test_static_sized_alloc()
{
  auto& alloc = ThreadAlloc::get();
  auto p = alloc.alloc<asz>();

  static_assert((dealloc >= 0) && (dealloc <= 2), "bad dealloc flavor");
  switch (dealloc)
  {
    case 0:
      alloc.dealloc(p);
      break;
    case 1:
      alloc.dealloc(p, asz);
      break;
    case 2:
      alloc.dealloc<asz>(p);
      break;
  }
}

void test_static_sized_allocs()
{
  // For each small, medium, and large class, do each kind dealloc.  This is
  // mostly to ensure that all of these forms compile.
  for (size_t sc = 0; sc < NUM_SMALL_SIZECLASSES; sc++)
  {
    // test_static_sized_alloc<sc, 0>();
    // test_static_sized_alloc<sc, 1>();
    // test_static_sized_alloc<sc, 2>();
  }
  // test_static_sized_alloc<sizeclass_to_size(NUM_SMALL_CLASSES + 1), 0>();
  // test_static_sized_alloc<sizeclass_to_size(NUM_SMALL_CLASSES + 1), 1>();
  // test_static_sized_alloc<sizeclass_to_size(NUM_SMALL_CLASSES + 1), 2>();

  // test_static_sized_alloc<large_sizeclass_to_size(0), 0>();
  // test_static_sized_alloc<large_sizeclass_to_size(0), 1>();
  // test_static_sized_alloc<large_sizeclass_to_size(0), 2>();
}

void test_remaining_bytes()
{
  auto& alloc = ThreadAlloc::get();
  for (snmalloc::smallsizeclass_t sc = size_to_sizeclass(MIN_ALLOC_SIZE);
       sc < NUM_SMALL_SIZECLASSES;
       sc++)
  {
    auto size = sizeclass_to_size(sc);
    char* p = (char*)alloc.alloc(size);
    for (size_t offset = 0; offset < size; offset++)
    {
      auto rem = alloc.remaining_bytes(address_cast(pointer_offset(p, offset)));
      if (rem != (size - offset))
      {
        printf(
          "Allocation size: %zu,  Offset: %zu,  Remaining bytes: %zu, "
          "Expected: %zu\n",
          size,
          offset,
          rem,
          size - offset);
        abort();
      }
    }
    alloc.dealloc(p);
  }
}

void test_consolidaton_bug()
{
  /**
   * Check for consolidation of various sizes, but allocating and deallocating,
   * then requesting larger sizes. See issue #506
   */
  auto& alloc = ThreadAlloc::get();

  for (size_t i = 0; i < 27; i++)
  {
    std::vector<void*> allocs;
    for (size_t j = 0; j < 4; j++)
    {
      allocs.push_back(alloc.alloc(bits::one_at_bit(i)));
    }
    for (auto a : allocs)
    {
      alloc.dealloc(a);
    }
  }
}

int main(int argc, char** argv)
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
#ifdef USE_SYSTEMATIC_TESTING
  opt::Opt opt(argc, argv);
  size_t seed = opt.is<size_t>("--seed", 0);
  Virtual::systematic_bump_ptr() += seed << 17;
#else
  UNUSED(argc, argv);
#endif
  test_alloc_dealloc_64k();
  test_random_allocation();
  test_calloc();
  test_double_alloc();
#ifndef SNMALLOC_PASS_THROUGH // Depends on snmalloc specific features
  test_remaining_bytes();
  test_static_sized_allocs();
  test_calloc_large_bug();
  test_external_pointer_stack();
  test_external_pointer_dealloc_bug();
  test_external_pointer_large();
  test_external_pointer();
  test_alloc_16M();
  test_calloc_16M();
#endif
  test_consolidaton_bug();
  return 0;
}
