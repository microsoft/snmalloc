#include <snmalloc/pal/pal.h>
#include <test/helpers.h>
#include <test/setup.h>

#include <snmalloc/override/new.cc>

using namespace snmalloc;

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

  snmalloc::debug_check_empty();
  return 0;
}
