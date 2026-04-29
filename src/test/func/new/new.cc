#if defined(SNMALLOC_USE_SELF_VENDORED_STL) || \
  defined(SNMALLOC_THREAD_SANITIZER_ENABLED)
int main()
{
  return 0;
}
#else
#  include <array>
#  include <snmalloc/override/new.cc>
#  include <snmalloc/pal/pal.h>
#  include <test/helpers.h>
#  include <test/setup.h>

// snmalloc implements operator new[] / operator delete[] identically to their
// non-array counterparts, which causes GCC to produce false-positive
// -Wmismatched-new-delete warnings when it inlines through lambdas.
#  if defined(__GNUC__) && !defined(__clang__)
#    pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#  endif

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

template<typename New_Func, typename Del_Func>
void test_zero_alloc(New_Func new_func, Del_Func del_func)
{
  void* non_zero = new_func(0);
  EXPECT(non_zero, "allocation with size '0' did not return a valid pointer");
  del_func(non_zero);
}

// SNMALLOC_EXPORT void* operator new(size_t size)
// SNMALLOC_EXPORT void* operator new[](size_t size)
// SNMALLOC_EXPORT void operator delete(void* p) EXCEPTSPEC
// SNMALLOC_EXPORT void* operator delete[](void* p) EXCEPTSPEC
template<typename New_Func, typename Del_Func>
void test_new_delete_simple(New_Func new_fun, Del_Func del_fun, size_t size)
{
  void* non_zero = new_fun(size);
  bool caught_bad_alloc = false;
  EXPECT(non_zero, "expected valid address, but instead got {}", non_zero);
  del_fun(non_zero);

  test_zero_alloc(new_fun, del_fun);

  try
  {
    void* impossible_alloc = new_fun(static_cast<size_t>(-1));
    del_fun(impossible_alloc);
  }
  catch (std::bad_alloc&)
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
template<typename New_Func, typename Del_Func>
void test_new_delete_nothrow(New_Func new_fun, Del_Func del_fun)
{
  void* impossible_alloc_unaligned = new_fun(static_cast<size_t>(-1));
  EXPECT(
    impossible_alloc_unaligned == nullptr,
    "Impossible allocation should not succeed");
  del_fun(impossible_alloc_unaligned);

  test_zero_alloc(new_fun, del_fun);
}

// SNMALLOC_EXPORT void operator delete(void* p, size_t size) EXCEPTSPEC
// SNMALLOC_EXPORT void operator delete[](void* p, size_t size) EXCEPTSPEC
template<typename New_Func, typename Del_Func>
void test_delete_size(New_Func new_fun, Del_Func del_fun, size_t size)
{
  void* non_zero = new_fun(size);
  EXPECT(non_zero, "expected valid address, but instead got {}", non_zero);
  del_fun(non_zero, size);
}

// SNMALLOC_EXPORT void* operator new(size_t size, std::align_val_t val)
// SNMALLOC_EXPORT void* operator new[](size_t size, std::align_val_t val)
// SNMALLOC_EXPORT void operator delete(void* p, std::align_val_t) EXCEPTSPEC
// SNMALLOC_EXPORT void operator delete[](void* p, std::align_val_t) EXCEPTSPEC
template<typename New_Func, typename Del_Func>
void test_new_delete_aligned(New_Func new_fun, Del_Func del_fun, size_t size)
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

    test_zero_alloc(
      [&align_val, &new_fun](size_t s) { return new_fun(s, align_val); },
      [&align_val, &del_fun](void* p) { del_fun(p, align_val); });
  }
}

// SNMALLOC_EXPORT void* operator new(size_t size, std::align_val_t val, const
// std::nothrow_t&) noexcept
// SNMALLOC_EXPORT void* operator new[](size_t size,
// std::align_val_t val, const std::nothrow_t&) noexcept
template<typename New_Func, typename Del_Func>
void test_new_delete_aligned_nothrow(New_Func new_fun, Del_Func del_fun)
{
  std::align_val_t page_size{OS_PAGE_SIZE};
  void* impossible_alloc_aligned = new_fun(static_cast<size_t>(-1), page_size);
  EXPECT(
    impossible_alloc_aligned == nullptr,
    "Impossible allocation should not succeed");
  del_fun(impossible_alloc_aligned, page_size);

  test_zero_alloc(
    [&page_size, &new_fun](size_t s) { return new_fun(s, page_size); },
    [&page_size, &del_fun](void* p) { del_fun(p, page_size); });
}

// SNMALLOC_EXPORT void operator delete(void* p, size_t size, std::align_val_t
// val) EXCEPTSPEC
// SNMALLOC_EXPORT void operator delete[](void* p, size_t size,
// std::align_val_t val) EXCEPTSPEC
template<typename New_Func, typename Del_Func>
void test_delete_size_aligned(New_Func new_fun, Del_Func del_fun, size_t size)
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

  // Diagnostic: direct call to operator new outside any template/lambda
  // to verify whether snmalloc's override is reached at all.
  START_TEST("Test direct operator new call");
  snmalloc::message("About to call operator new(42) directly\n");
  void* diag_p = operator new(42);
  snmalloc::message("operator new(42) returned {}\n", diag_p);
  operator delete(diag_p);
  snmalloc::message("operator delete succeeded\n");

  // Check if operator new is the snmalloc version by attempting
  // an impossible allocation and seeing if it throws.
  START_TEST("Test direct impossible allocation");
  bool direct_bad_alloc = false;
  try
  {
    snmalloc::message("About to call operator new(size_t(-1)) directly\n");
    void* diag_impossible = operator new(static_cast<size_t>(-1));
    snmalloc::message(
      "operator new(size_t(-1)) returned {} (should have thrown!)\n",
      diag_impossible);
    operator delete(diag_impossible);
  }
  catch (std::bad_alloc&)
  {
    direct_bad_alloc = true;
    snmalloc::message("Caught std::bad_alloc from direct call\n");
  }
  catch (...)
  {
    snmalloc::message("Caught unknown exception from direct call\n");
  }
  snmalloc::message("direct_bad_alloc = {}\n", direct_bad_alloc);

  START_TEST("Test new / delete simple");
  test_new_delete_simple(
    [](size_t s) { return operator new(s); },
    [](void* p) { operator delete(p); },
    42);
  START_TEST("Test new[] / delete[] simple");
  test_new_delete_simple(
    [](size_t s) { return operator new[](s); },
    [](void* p) { operator delete[](p); },
    42);

  START_TEST("Test new / delete nothrow");
  test_new_delete_nothrow(
    [](size_t s) { return operator new(s, std::nothrow); },
    [](void* p) { operator delete(p, std::nothrow); });
  START_TEST("Test new[] / delete[] nothrow");
  test_new_delete_nothrow(
    [](size_t s) { return operator new[](s, std::nothrow); },
    [](void* p) { operator delete[](p, std::nothrow); });

  START_TEST("Test delete with size parameter");
  test_delete_size(
    [](size_t s) { return operator new(s); },
    [](void* p, size_t sz) { operator delete(p, sz); },
    42);
  START_TEST("Test delete[] with size parameter");
  test_delete_size(
    [](size_t s) { return operator new[](s); },
    [](void* p, size_t sz) { operator delete[](p, sz); },
    42);

  START_TEST("Test new / delete aligned");
  test_new_delete_aligned(
    [](size_t s, std::align_val_t a) { return operator new(s, a); },
    [](void* p, std::align_val_t a) { operator delete(p, a); },
    42);
  START_TEST("Test new[] / delete[] aligned");
  test_new_delete_aligned(
    [](size_t s, std::align_val_t a) { return operator new[](s, a); },
    [](void* p, std::align_val_t a) { operator delete[](p, a); },
    42);

  START_TEST("Test non-throwing aligned new / delete");
  test_new_delete_aligned_nothrow(
    [](size_t s, std::align_val_t a) {
      return operator new(s, a, std::nothrow);
    },
    [](void* p, std::align_val_t a) { operator delete(p, a, std::nothrow); });
  START_TEST("Test non-throwing aligned new[] / delete[]");
  test_new_delete_aligned_nothrow(
    [](size_t s, std::align_val_t a) {
      return operator new[](s, a, std::nothrow);
    },
    [](void* p, std::align_val_t a) { operator delete[](p, a, std::nothrow); });

  START_TEST("Test non-throwing aligned delete with explicit size");
  test_delete_size_aligned(
    [](size_t s, std::align_val_t a) { return operator new(s, a); },
    [](void* p, size_t sz, std::align_val_t a) { operator delete(p, sz, a); },
    42);
  START_TEST("Test non-throwing aligned delete[] with explicit size");
  test_delete_size_aligned(
    [](size_t s, std::align_val_t a) { return operator new[](s, a); },
    [](void* p, size_t sz, std::align_val_t a) { operator delete[](p, sz, a); },
    42);

  snmalloc::debug_check_empty();
  return 0;
}
#endif
