/**
 * The first operation a thread performs takes a different path to every
 * subsequent operation as it must lazily initialise the thread local allocator.
 * This tests performs all sizes of allocation, and deallocation as the first
 * operation.
 */

#include "test/setup.h"

#include <iostream>
#include <snmalloc/snmalloc.h>
#include <thread>

void alloc1(size_t size)
{
  void* r = snmalloc::ThreadAlloc::get().alloc(size);
  snmalloc::ThreadAlloc::get().dealloc(r);
}

void alloc2(size_t size)
{
  auto& a = snmalloc::ThreadAlloc::get();
  void* r = a.alloc(size);
  a.dealloc(r);
}

void alloc3(size_t size)
{
  auto& a = snmalloc::ThreadAlloc::get();
  void* r = a.alloc(size);
  a.dealloc(r, size);
}

void alloc4(size_t size)
{
  auto& a = snmalloc::ThreadAlloc::get();
  void* r = a.alloc(size);
  a.dealloc(r);
}

void check_calloc(void* p, size_t size)
{
  if (p != nullptr)
  {
    for (size_t i = 0; i < size; i++)
    {
      if (((uint8_t*)p)[i] != 0)
      {
        std::cout << "Calloc contents:" << std::endl;
        for (size_t j = 0; j < size; j++)
        {
          std::cout << std::hex << (size_t)((uint8_t*)p)[j] << " ";
          if (j % 32 == 0)
            std::cout << std::endl;
        }
        abort();
      }
      //      ((uint8_t*)p)[i] = 0x5a;
    }
  }
}

void calloc1(size_t size)
{
  void* r =
    snmalloc::ThreadAlloc::get().alloc<snmalloc::ZeroMem::YesZero>(size);
  check_calloc(r, size);
  snmalloc::ThreadAlloc::get().dealloc(r);
}

void calloc2(size_t size)
{
  auto& a = snmalloc::ThreadAlloc::get();
  void* r = a.alloc<snmalloc::ZeroMem::YesZero>(size);
  check_calloc(r, size);
  a.dealloc(r);
}

void calloc3(size_t size)
{
  auto& a = snmalloc::ThreadAlloc::get();
  void* r = a.alloc<snmalloc::ZeroMem::YesZero>(size);
  check_calloc(r, size);
  a.dealloc(r, size);
}

void calloc4(size_t size)
{
  auto& a = snmalloc::ThreadAlloc::get();
  void* r = a.alloc<snmalloc::ZeroMem::YesZero>(size);
  check_calloc(r, size);
  a.dealloc(r);
}

void dealloc1(void* p, size_t)
{
  snmalloc::ThreadAlloc::get().dealloc(p);
}

void dealloc2(void* p, size_t size)
{
  snmalloc::ThreadAlloc::get().dealloc(p, size);
}

void dealloc3(void* p, size_t)
{
  snmalloc::ThreadAlloc::get().dealloc(p);
}

void dealloc4(void* p, size_t size)
{
  snmalloc::ThreadAlloc::get().dealloc(p, size);
}

void f(size_t size)
{
  auto t1 = std::thread(alloc1, size);
  auto t2 = std::thread(alloc2, size);
  auto t3 = std::thread(alloc3, size);
  auto t4 = std::thread(alloc4, size);

  auto t5 = std::thread(calloc1, size);
  auto t6 = std::thread(calloc2, size);
  auto t7 = std::thread(calloc3, size);
  auto t8 = std::thread(calloc4, size);

  {
    auto a = snmalloc::get_scoped_allocator();
    auto p1 = a->alloc(size);
    auto p2 = a->alloc(size);
    auto p3 = a->alloc(size);
    auto p4 = a->alloc(size);

    auto t9 = std::thread(dealloc1, p1, size);
    auto t10 = std::thread(dealloc2, p2, size);
    auto t11 = std::thread(dealloc3, p3, size);
    auto t12 = std::thread(dealloc4, p4, size);

    t1.join();
    t2.join();
    t3.join();
    t4.join();
    t5.join();
    t6.join();
    t7.join();
    t8.join();
    t9.join();
    t10.join();
    t11.join();
    t12.join();
  } // Drops a.
  //  snmalloc::current_alloc_pool()->debug_in_use(0);
  printf(".");
  fflush(stdout);
}

int main(int, char**)
{
  setup();
  printf(".");
  fflush(stdout);

  f(0);
  f(1);
  f(3);
  f(5);
  f(7);
  printf("\n");
  for (size_t exp = 1; exp < snmalloc::MAX_SMALL_SIZECLASS_BITS; exp++)
  {
    auto shifted = [exp](size_t v) { return v << exp; };

    f(shifted(1));
    f(shifted(3));
    f(shifted(5));
    f(shifted(7));
    f(shifted(1) + 1);
    f(shifted(3) + 1);
    f(shifted(5) + 1);
    f(shifted(7) + 1);
    f(shifted(1) - 1);
    f(shifted(3) - 1);
    f(shifted(5) - 1);
    f(shifted(7) - 1);
    printf("\n");
  }
}
