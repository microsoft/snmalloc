// clang-format off
#include <string>
#include <vector>
#include "../../../override/rust.cc"
// clang-format on
template<bool local_clear, typename F>
SNMALLOC_SLOW_PATH void test_rust_global_allocate(F func)
{
  size_t size = 32 * Pal::page_size;
  auto x = static_cast<char*>(func(Pal::page_size, size));
  if constexpr (local_clear)
  {
    Pal::zero(x, size);
  }
  SNMALLOC_ASSERT(
    *x == 0 && 0 == std::memcmp(x, x + 1, 32 * Pal::page_size - 1));
  for (size_t i = 1; i < size; i <<= 1)
  {
    x[i] = 1;
  }
  x = static_cast<char*>(sn_rust_realloc(x, Pal::page_size, size, 2 * size));
  bool result = true;
  snmalloc::UNUSED(result);
  for (size_t i = 1; i < size; i++)
  {
    result &= bits::is_pow2(i) ? (x[i] == 1) : (x[i] == 0);
  }
  SNMALLOC_ASSERT(x[0] == 0 && result);
  sn_rust_dealloc(x, Pal::page_size, 2 * size);
}

template<class T>
class RAllocator
{
public:
  using value_type = T;
  using size_type = std::size_t;
  using different_type = std::ptrdiff_t;
  using propagate_on_container_move_assignment = std::true_type;

  RAllocator() : alloc(sn_rust_allocator_new()) {}
  RAllocator(const RAllocator&) : alloc(sn_rust_allocator_new()) {}
  template<class U>
  explicit operator RAllocator<U>()
  {
    return {};
  }
  RAllocator(RAllocator&& that) noexcept : alloc(that.alloc)
  {
    that.alloc = nullptr;
  };
  ~RAllocator()
  {
    if (alloc)
      sn_rust_allocator_drop(alloc);
  }
  [[nodiscard]] T* allocate(std::size_t n)
  {
    return static_cast<T*>(
      sn_rust_allocator_allocate(alloc, alignof(T), n * sizeof(T)));
  };

  void deallocate(T* p, std::size_t n)
  {
    return sn_rust_allocator_deallocate(alloc, p, alignof(T), n * sizeof(T));
  };

private:
  Alloc* alloc;

  template<class T1, class T2>
  friend bool
  operator==(const RAllocator<T1>& lhs, const RAllocator<T2>& rhs) noexcept;
};

template<class T1, class T2>
bool operator==(const RAllocator<T1>& lhs, const RAllocator<T2>& rhs) noexcept
{
  return lhs.alloc == rhs.alloc;
}

void test_allocator_vector()
{
  using Allocator = RAllocator<std::string>;
  using Vector = std::vector<std::string, Allocator>;
  Vector origin;
  for (auto i = 'a'; i <= 'z'; ++i)
  {
    origin.emplace_back(i, i);
  }
  Vector cloned = origin;
  Vector moved = std::move(origin);

  for (size_t i = 'a'; i <= 'z'; ++i)
  {
    SNMALLOC_ASSERT(cloned[i - 'a'] == std::string(i, i));
    SNMALLOC_ASSERT(moved[i - 'a'] == std::string(i, i));
  }

  for (size_t i = 'A'; i <= 'Z'; ++i)
  {
    cloned.emplace_back(i, i);
    moved.emplace_back(i, i);
  }

  std::swap(cloned, moved);

  for (size_t i = 'a'; i <= 'z'; ++i)
  {
    SNMALLOC_ASSERT(cloned[i - 'a'] == std::string(i, i));
    SNMALLOC_ASSERT(moved[i - 'a'] == std::string(i, i));
  }

  for (size_t i = 'A'; i <= 'Z'; ++i)
  {
    SNMALLOC_ASSERT(cloned[i - 'A' + ('z' - 'a' + 1)] == std::string(i, i));
    SNMALLOC_ASSERT(moved[i - 'A' + ('z' - 'a' + 1)] == std::string(i, i));
  }
}

int main()
{
  test_rust_global_allocate<true>(sn_rust_alloc);
  test_rust_global_allocate<false>(sn_rust_alloc_zeroed);
  test_allocator_vector();
}
