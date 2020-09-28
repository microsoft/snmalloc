#include <iostream>
#include <snmalloc.h>
#include <test/opt.h>
#include <test/setup.h>
#include <test/xoroshiro.h>
#include <unordered_set>
#if defined(__linux__) && !defined(SNMALLOC_QEMU_WORKAROUND)
/*
 * We only test allocations with limited AS on linux for now.
 * It should be a good representative for POSIX systems.
 * QEMU `setrlimit64` does not behave as the same as native linux,
 * so we need to exclude it from such tests.
 */
#  include <sys/resource.h>
#  include <sys/sysinfo.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  define TEST_LIMITED
#  define KiB (1024ull)
#  define MiB (KiB * KiB)
#  define GiB (KiB * MiB)
#endif

using namespace snmalloc;

#ifdef TEST_LIMITED
void test_limited(rlim64_t as_limit, size_t& count)
{
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
    auto alloc = ThreadAlloc::get();
    std::cout << "allocator initialised" << std::endl;
    auto chunk = alloc->alloc(upper_bound);
    alloc->dealloc(chunk);
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
}
#endif

void test_alloc_dealloc_64k()
{
  auto alloc = ThreadAlloc::get();

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
      garbage[i] = alloc->alloc(16);
    }

    // Allocate one object on the second slab
    keep_alive[j] = alloc->alloc(16);

    for (size_t i = 0; i < count; i++)
    {
      alloc->dealloc(garbage[i]);
    }
  }
  for (size_t j = 0; j < outer_count; j++)
  {
    alloc->dealloc(keep_alive[j]);
  }
}

void test_random_allocation()
{
  auto alloc = ThreadAlloc::get();
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
        alloc->dealloc(cell);
        allocated.erase(cell);
        cell = nullptr;
        alloc_count--;
      }
      if (!just_dealloc)
      {
        cell = alloc->alloc(16);
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
      alloc->dealloc(objects[i]);
}

void test_calloc()
{
  auto alloc = ThreadAlloc::get();

  for (size_t size = 16; size <= (1 << 24); size <<= 1)
  {
    void* p = alloc->alloc(size);
    memset(p, 0xFF, size);
    alloc->dealloc(p, size);

    p = alloc->alloc<YesZero>(size);

    for (size_t i = 0; i < size; i++)
    {
      if (((char*)p)[i] != 0)
        abort();
    }

    alloc->dealloc(p, size);
  }

  current_alloc_pool()->debug_check_empty();
}

void test_double_alloc()
{
  auto* a1 = current_alloc_pool()->acquire();
  auto* a2 = current_alloc_pool()->acquire();

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

  current_alloc_pool()->release(a1);
  current_alloc_pool()->release(a2);
  current_alloc_pool()->debug_check_empty();
}

void test_external_pointer()
{
  // Malloc does not have an external pointer querying mechanism.
  auto alloc = ThreadAlloc::get();

  for (uint8_t sc = 0; sc < NUM_SIZECLASSES; sc++)
  {
    size_t size = sizeclass_to_size(sc);
    void* p1 = alloc->alloc(size);

    for (size_t offset = 0; offset < size; offset += 17)
    {
      void* p2 = pointer_offset(p1, offset);
      void* p3 = Alloc::external_pointer(p2);
      void* p4 = Alloc::external_pointer<End>(p2);
      UNUSED(p3);
      UNUSED(p4);
      SNMALLOC_CHECK(p1 == p3);
      SNMALLOC_CHECK((size_t)p4 == (size_t)p1 + size - 1);
    }

    alloc->dealloc(p1, size);
  }

  current_alloc_pool()->debug_check_empty();
};

void check_offset(void* base, void* interior)
{
  void* calced_base = Alloc::external_pointer((void*)interior);
  if (calced_base != (void*)base)
    abort();
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

  auto alloc = ThreadAlloc::get();

  constexpr size_t count_log = snmalloc::bits::is64() ? 5 : 3;
  constexpr size_t count = 1 << count_log;
  // Pre allocate all the objects
  size_t* objects[count];

  size_t total_size = 0;

  for (size_t i = 0; i < count; i++)
  {
    size_t b = SUPERSLAB_BITS + 3;
    size_t rand = r.next() & ((1 << b) - 1);
    size_t size = (1 << 24) + rand;
    total_size += size;
    // store object
    objects[i] = (size_t*)alloc->alloc(size);
    // Store allocators size for this object
    *objects[i] = Alloc::alloc_size(objects[i]);

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
    alloc->dealloc(objects[i]);
  }
}

void test_external_pointer_dealloc_bug()
{
  auto alloc = ThreadAlloc::get();
  constexpr size_t count = (SUPERSLAB_SIZE / SLAB_SIZE) * 2;
  void* allocs[count];

  for (size_t i = 0; i < count; i++)
  {
    allocs[i] = alloc->alloc(SLAB_SIZE / 2);
  }

  for (size_t i = 1; i < count; i++)
  {
    alloc->dealloc(allocs[i]);
  }

  for (size_t i = 0; i < count; i++)
  {
    Alloc::external_pointer(allocs[i]);
  }

  alloc->dealloc(allocs[0]);
}

void test_alloc_16M()
{
  auto alloc = ThreadAlloc::get();
  // sizes >= 16M use large_alloc
  const size_t size = 16'000'000;

  void* p1 = alloc->alloc(size);
  SNMALLOC_CHECK(Alloc::alloc_size(Alloc::external_pointer(p1)) >= size);
  alloc->dealloc(p1);
}

void test_calloc_16M()
{
  auto alloc = ThreadAlloc::get();
  // sizes >= 16M use large_alloc
  const size_t size = 16'000'000;

  void* p1 = alloc->alloc<YesZero>(size);
  SNMALLOC_CHECK(Alloc::alloc_size(Alloc::external_pointer(p1)) >= size);
  alloc->dealloc(p1);
}

void test_calloc_large_bug()
{
  auto alloc = ThreadAlloc::get();
  // Perform large calloc, to check for correct zeroing from PAL.
  // Some PALS have special paths for PAGE aligned zeroing of large
  // allocations.  This is a large allocation that is intentionally
  // not a multiple of page size.
  const size_t size = (SUPERSLAB_SIZE << 3) - 7;

  void* p1 = alloc->alloc<YesZero>(size);
  SNMALLOC_CHECK(Alloc::alloc_size(Alloc::external_pointer(p1)) >= size);
  alloc->dealloc(p1);
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
  UNUSED(argc);
  UNUSED(argv);
#endif

  test_alloc_dealloc_64k();
  test_random_allocation();
  test_calloc();
  test_double_alloc();
#ifndef SNMALLOC_PASS_THROUGH // Depends on snmalloc specific features
  test_calloc_large_bug();
  test_external_pointer_dealloc_bug();
  test_external_pointer_large();
  test_external_pointer();
  test_alloc_16M();
  test_calloc_16M();
#endif
  return 0;
}
