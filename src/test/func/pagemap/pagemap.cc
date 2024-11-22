/**
 * Unit tests for operations in pagemap operations.
 *
 * Currently this tests a very specific case where the pagemap
 * requires multiple levels of index. This was incorrectly implemented,
 * but no examples were using multiple levels of pagemap.
 */

#include <iostream>
#include <snmalloc/snmalloc.h>
#include <test/setup.h>

using namespace snmalloc;
static constexpr size_t GRANULARITY_BITS = 20;

struct T
{
  size_t v = 99;

  T(size_t v) : v(v) {}

  T() {}
};

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
    std::cout << "Location: " << std::hex << address << " Read: " << value.v
              << " Expected: " << expected.v << " on " << file << ":" << lineno
              << std::endl;
    failure_count++;
  }
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
  address_t high = bits::one_at_bit(29);
  void* base = nullptr;

  // Nullptr needs to work before initialisation
  CHECK_GET(bounded, 0, T());

  // Initialise the pagemap
  if (bounded)
  {
    auto size = bits::one_at_bit(29);
    base = DefaultPal::reserve(size);
    DefaultPal::notify_using<NoZero>(base, size);
    std::cout << "Fixed base: " << base << " (" << size << ") "
              << " end: " << pointer_offset(base, size) << std::endl;
    auto [heap_base, heap_size] = pagemap_test_bound.init(base, size);
    std::cout << "Heap base:  " << heap_base << " (" << heap_size << ") "
              << " end: " << pointer_offset(heap_base, heap_size) << std::endl;
    low = address_cast(heap_base);
    base = heap_base;
    high = low + heap_size;
    // Store a pattern in heap.
    memset(base, 0x23, high - low);
  }
  else
  {
    static constexpr bool pagemap_randomize =
      mitigations(random_pagemap) && !aal_supports<StrictProvenance>;

    pagemap_test_unbound.init<pagemap_randomize>();
    pagemap_test_unbound.register_range(low, high - low);
  }

  // Nullptr should still work after init.
  CHECK_GET(bounded, 0, T());

  // Store a pattern into page map
  T value = 1;
  for (address_t ptr = low; ptr < high;
       ptr += bits::one_at_bit(GRANULARITY_BITS + 3))
  {
    set(bounded, ptr, value);
    value.v++;
    if (value.v == T().v)
      value = 0;
    if (((ptr - low) % (1ULL << 26)) == 0)
      std::cout << "." << std::flush;
  }

  // Check pattern is correctly stored
  std::cout << std::endl;

  if (bounded)
  {
    std::cout << "Checking heap" << std::endl;
    // Check we have not corrupted the heap.
    for (size_t offset = 0; offset < high - low; offset++)
    {
      if ((offset % (1ULL << 26)) == 0)
        std::cout << "." << std::flush;
      auto* p = ((char*)base) + offset;
      if (*p != 0x23)
      {
        printf("Heap and pagemap have collided at %p", p);
        abort();
      }
    }

    std::cout << std::endl;
    std::cout << "Storing new pattern" << std::endl;
    // Store a different pattern in heap.
    memset(base, 0x23, high - low);
  }

  std::cout << "Checking pagemap contents" << std::endl;
  value = 1;
  for (address_t ptr = low; ptr < high;
       ptr += bits::one_at_bit(GRANULARITY_BITS + 3))
  {
    CHECK_GET(bounded, ptr, value);
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
  UNUSED(argc, argv);

  setup();

  test_pagemap(false);
  test_pagemap(true);

  if (failure_count != 0)
  {
    std::cout << "Failure count: " << failure_count << std::endl;
    abort();
  }
}
