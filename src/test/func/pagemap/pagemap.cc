/**
 * Unit tests for operations in pagemap operations.
 *
 * Currently this tests a very specific case where the pagemap
 * requires multiple levels of index. This was incorrectly implemented,
 * but no examples were using multiple levels of pagemap.
 */

#include <backend/pagemap.h>
#include <ds/bits.h>
#include <iostream>
#include <snmalloc.h>
#include <test/setup.h>

using namespace snmalloc;
static constexpr size_t GRANULARITY_BITS = 20;
struct T
{
  size_t v = 99;
  T(size_t v) : v(v) {}
  T() {}
};

AddressSpaceManager<DefaultPal> address_space;

FlatPagemap<GRANULARITY_BITS, T, DefaultPal, false> pagemap_test_unbound;

FlatPagemap<GRANULARITY_BITS, T, DefaultPal, true> pagemap_test_bound;

size_t failure_count = 0;

void check_get(
  bool bounded, address_t address, T expected, const char* file, size_t lineno)
{
  T value = 0;
  if (bounded)
    value = pagemap_test_bound.get<false>(address);
  else
    value = pagemap_test_unbound.get<false>(address);

  if (value.v != expected.v)
  {
    std::cout << "Location: " << (void*)address << " Read: " << value.v
              << " Expected: " << expected.v << " on " << file << ":" << lineno
              << std::endl;
    failure_count++;
  }
}

void add(bool bounded, address_t address, T new_value)
{
  if (bounded)
    pagemap_test_bound.add(address, new_value);
  else
    pagemap_test_unbound.add(address, new_value);
}

void set(bool bounded, address_t address, T new_value)
{
  if (bounded)
    pagemap_test_bound.set(address, new_value);
  else
    pagemap_test_unbound.set(address, new_value);
}

#define CHECK_GET(b, a, e) check_get(b, a, e, __FILE__, __LINE__)

void test_pagemap(bool bounded)
{
  address_t low = bits::one_at_bit(23);
  address_t high = bits::one_at_bit(30);

  // Nullptr needs to work before initialisation
  CHECK_GET(true, 0, T());

  // Initialise the pagemap
  if (bounded)
  {
    auto size = bits::one_at_bit(30);
    auto base = address_space.reserve<true>(size);
    std::cout << "Fixed base: " << base.unsafe_ptr() << " (" << size << ") "
              << " end: " << pointer_offset(base, size).unsafe_ptr()
              << std::endl;
    auto [heap_base, heap_size] = pagemap_test_bound.init(base, size);
    std::cout << "Heap base:  " << heap_base.unsafe_ptr() << " (" << heap_size
              << ") "
              << " end: " << pointer_offset(heap_base, heap_size).unsafe_ptr()
              << std::endl;
    low = address_cast(heap_base);
    high = low + heap_size;
  }
  else
  {
    pagemap_test_unbound.init();
  }

  // Nullptr should still work after init.
  CHECK_GET(true, 0, T());

  // Store a pattern into page map
  T value = 1;
  for (uintptr_t ptr = low; ptr < high;
       ptr += bits::one_at_bit(GRANULARITY_BITS + 3))
  {
    add(false, ptr, value);
    value.v++;
    if (value.v == T().v)
      value = 0;
    if (((ptr - low) % (1ULL << 26)) == 0)
      std::cout << "." << std::flush;
  }

  // Check pattern is correctly stored
  std::cout << std::endl;
  value = 1;
  for (uintptr_t ptr = low; ptr < high;
       ptr += bits::one_at_bit(GRANULARITY_BITS + 3))
  {
    CHECK_GET(false, ptr, value);
    value.v++;
    if (value.v == T().v)
      value = 0;

    if (((ptr - low) % (1ULL << 26)) == 0)
      std::cout << "." << std::flush;
  }
  std::cout << std::endl;
}

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);

  setup();

  test_pagemap(false);
  test_pagemap(true);

  if (failure_count != 0)
  {
    std::cout << "Failure count: " << failure_count << std::endl;
    abort();
  }
}
