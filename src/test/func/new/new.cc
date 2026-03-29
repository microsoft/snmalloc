#include <snmalloc/pal/pal.h>
#include <test/helpers.h>
#include <test/setup.h>

#include <snmalloc/override/new.cc>

using namespace snmalloc;

constexpr std::array<size_t, 11> align_val_sizes = {
  8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, OS_PAGE_SIZE};

void test_delete_null()
{
  operator delete(nullptr, 42);
  operator delete(nullptr, std::nothrow);
  operator delete(nullptr, 42, std::align_val_t{OS_PAGE_SIZE});
  operator delete(nullptr, std::align_val_t{OS_PAGE_SIZE});
  operator delete(nullptr, std::align_val_t{OS_PAGE_SIZE}, std::nothrow);
  operator delete[](nullptr);
  operator delete[](nullptr, 42);
  operator delete[](nullptr, std::nothrow);
  operator delete[](nullptr, std::align_val_t{OS_PAGE_SIZE});
  operator delete[](nullptr, 42, std::align_val_t{OS_PAGE_SIZE});
  operator delete[](nullptr, std::align_val_t{OS_PAGE_SIZE}, std::nothrow);
}

template<typename New_Func>
void test_zero_alloc(New_Func new_func)
{
  void* non_zero = new_func(0);
  EXPECT(non_zero, "allocation with size '0' did not return a valid pointer");
  operator delete(non_zero);
}

// SNMALLOC_EXPORT void* operator new(size_t size)
// SNMALLOC_EXPORT void* operator new[](size_t size)
// SNMALLOC_EXPORT void operator delete(void* p) EXCEPTSPEC
// SNMALLOC_EXPORT void* operator delete[](void* p) EXCEPTSPEC
template<void* (*new_fun)(size_t), void (*del_fun)(void*) EXCEPTSPEC>
void test_new_delete_simple(size_t size)
{
  void* non_zero = new_fun(size);
  bool caught_bad_alloc = false;
  EXPECT(non_zero, "expected valid address, but instead got {}", non_zero);
  del_fun(non_zero);

  test_zero_alloc(new_fun);

  try
  {
    void* impossible_alloc = new_fun(static_cast<size_t>(-1));
    del_fun(impossible_alloc);
  }
  catch (std::bad_alloc& e)
  {
    caught_bad_alloc = true;
  }

  EXPECT(
    caught_bad_alloc,
    "Impossible allocation did not throw std::bad_alloc exception");
}

// SNMALLOC_EXPORT void* operator new(size_t size, const std::nothrow_t&)
// noexcept
// SNMALLOC_EXPORT void* operator new[](size_t size, const std::nothrow_t&)
// noexcept
// SNMALLOC_EXPORT void* operator delete(size_t size, const std::nothrow_t&)
// noexcept
// SNMALLOC_EXPORT void* operator delete[](size_t size, const std::nothrow_t&)
// noexcept
template<
  void* (*new_fun)(size_t, const std::nothrow_t&),
  void (*del_fun)(void*, const std::nothrow_t&) EXCEPTSPEC>
void test_new_delete_nothrow()
{
  void* impossible_alloc_unaligned =
    new_fun(static_cast<size_t>(-1), std::nothrow);
  EXPECT(
    impossible_alloc_unaligned == nullptr,
    "Impossible allocation should not succeed");
  del_fun(impossible_alloc_unaligned, std::nothrow);

  test_zero_alloc([](size_t s) { return new_fun(s, std::nothrow); });
}

// SNMALLOC_EXPORT void operator delete(void* p, size_t size) EXCEPTSPEC
// SNMALLOC_EXPORT void operator delete[](void* p, size_t size) EXCEPTSPEC
template<void* (*new_fun)(size_t), void (*del_fun)(void*, size_t) EXCEPTSPEC>
void test_delete_size(size_t size)
{
  void* non_zero = new_fun(size);
  EXPECT(non_zero, "expected valid address, but instead got {}", non_zero);
  del_fun(non_zero, size);
}

// SNMALLOC_EXPORT void* operator new(size_t size, std::align_val_t val)
// SNMALLOC_EXPORT void* operator new[](size_t size, std::align_val_t val)
// SNMALLOC_EXPORT void operator delete(void* p, std::align_val_t) EXCEPTSPEC
// SNMALLOC_EXPORT void operator delete[](void* p, std::align_val_t) EXCEPTSPEC
template<
  void* (*new_fun)(size_t, std::align_val_t),
  void (*del_fun)(void*, std::align_val_t) EXCEPTSPEC>
void test_new_delete_aligned(size_t size)
{
  for (auto& align_val_size : align_val_sizes)
  {
    std::align_val_t align_val{align_val_size};
    void* aligned_mem = new_fun(size, align_val);
    EXPECT(
      is_aligned(aligned_mem, align_val_size),
      "Memory was not aligned on value {}",
      align_val_size);
    del_fun(aligned_mem, align_val);

    test_zero_alloc([&align_val](size_t s) { return new_fun(s, align_val); });
  }
}

// SNMALLOC_EXPORT void* operator new(size_t size, std::align_val_t val, const
// std::nothrow_t&) noexcept
// SNMALLOC_EXPORT void* operator new[](size_t size,
// std::align_val_t val, const std::nothrow_t&) noexcept
template<
  void* (*new_fun)(size_t, std::align_val_t, const std::nothrow_t&),
  void (*del_fun)(void*, std::align_val_t, const std::nothrow_t&) EXCEPTSPEC>
void test_new_delete_aligned_nothrow()
{
  std::align_val_t page_size{OS_PAGE_SIZE};
  void* impossible_alloc_aligned =
    new_fun(static_cast<size_t>(-1), page_size, std::nothrow);
  EXPECT(
    impossible_alloc_aligned == nullptr,
    "Impossible allocation should not succeed");
  del_fun(impossible_alloc_aligned, page_size, std::nothrow);

  test_zero_alloc(
    [&page_size](size_t s) { return new_fun(s, page_size, std::nothrow); });
}

// SNMALLOC_EXPORT void operator delete(void* p, size_t size, std::align_val_t
// val) EXCEPTSPEC
// SNMALLOC_EXPORT void operator delete[](void* p, size_t size,
// std::align_val_t val) EXCEPTSPEC
template<
  void* (*new_fun)(size_t, std::align_val_t),
  void (*del_fun)(void*, size_t, std::align_val_t) EXCEPTSPEC>
void test_delete_size_aligned(size_t size)
{
  for (auto& align_val_size : align_val_sizes)
  {
    std::align_val_t align_val{align_val_size};
    void* aligned_mem = new_fun(size, align_val);
    EXPECT(
      is_aligned(aligned_mem, align_val_size),
      "Memory was not aligned on value {}",
      align_val_size);
    del_fun(aligned_mem, size, align_val);
  }
}

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);

  setup();

  START_TEST("Test delete / delete[] nullptr");
  test_delete_null();

  START_TEST("Test new / delete simple");
  test_new_delete_simple < operator new, operator delete>(42);
  START_TEST("Test new[] / delete[] simple");
  test_new_delete_simple < operator new[], operator delete[]>(42);

  START_TEST("Test new / delete nothrow");
  test_new_delete_nothrow < operator new, operator delete>();
  START_TEST("Test new[] / delete[] nothrow");
  test_new_delete_nothrow < operator new[], operator delete[]>();

  START_TEST("Test delete with size parameter");
  test_delete_size < operator new, operator delete>(42);
  START_TEST("Test delete[] with size parameter");
  test_delete_size < operator new[], operator delete[]>(42);

  START_TEST("Test new / delete aligned");
  test_new_delete_aligned < operator new, operator delete>(42);
  START_TEST("Test new[] / delete[] aligned");
  test_new_delete_aligned < operator new[], operator delete[]>(42);

  START_TEST("Test non-throwing aligned new / delete");
  test_new_delete_aligned_nothrow < operator new, operator delete>();
  START_TEST("Test non-throwing aligned new[] / delete[]");
  test_new_delete_aligned_nothrow < operator new[], operator delete[]>();

  START_TEST("Test non-throwing aligned delete with explicit size");
  test_delete_size_aligned < operator new, operator delete>(42);
  START_TEST("Test non-throwing aligned delete[] with explicit size");
  test_delete_size_aligned < operator new[], operator delete[]>(42);

  snmalloc::debug_check_empty();
  return 0;
}
