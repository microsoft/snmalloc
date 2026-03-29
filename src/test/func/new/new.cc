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

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);

  setup();

  START_TEST("Test delete / delete[] nullptr");
  test_delete_null();

  snmalloc::debug_check_empty();
  return 0;
}
