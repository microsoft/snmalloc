// clang-format off
#include <string>
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
  for (size_t i = 1; i < size; i++)
  {
    result &= bits::is_pow2(i) ? (x[i] == 1) : (x[i] == 0);
  }
  SNMALLOC_ASSERT(x[0] == 0 && result);
  sn_rust_dealloc(x, Pal::page_size, 2 * size);
}

template<class T, bool used_zeroed>
class RAllocator
{
public:
  using value_type = T;
  using size_type = std::size_t;
  using different_type = std::ptrdiff_t;
  using propagate_on_container_move_assignment = std::true_type;
  template< class U > struct rebind {
    using other = RAllocator<U, used_zeroed>;
  };

  RAllocator() : alloc(sn_rust_allocator_new()) {}
  RAllocator(const RAllocator&) : alloc(sn_rust_allocator_new()) {}
  RAllocator(RAllocator&& that) noexcept : alloc(that.alloc)
  {
    that.alloc = nullptr;
  };
  [[nodiscard]] T* allocate(std::size_t n)
  {
    if constexpr (used_zeroed)
      return static_cast<T*>(
        sn_rust_allocator_allocate_zeroed(alloc, alignof(T), n * sizeof(T)));
    else
      return static_cast<T*>(
        sn_rust_allocator_allocate(alloc, alignof(T), n * sizeof(T)));
  };

  void deallocate(T* p, std::size_t n)
  {
    return sn_rust_allocator_deallocate(alloc, p, alignof(T), n * sizeof(T));
  };

private:
  Alloc* alloc;

  template<class T1, class T2, bool x, bool y>
  friend bool
  operator==(const RAllocator<T1, x>& lhs, const RAllocator<T2, y>& rhs) noexcept;
};

template<class T1, class T2, bool x, bool y>
bool operator==(const RAllocator<T1, x>& lhs, const RAllocator<T2, y>& rhs) noexcept
{
  return lhs.alloc == rhs.alloc && x == y;
};

template <bool use_zeored>
void test_allocator_vector()
{
  using Allocator = RAllocator<std::string, use_zeored>;
  using Vector = std::vector<std::string, Allocator>;
  Vector vector;
  for (auto i = 'a'; i <= 'z'; ++i)
  {
    vector.emplace_back(i, i);
  }
  Vector cloned = vector;
  Vector moved = std::move(vector);

  for (auto i = 'a'; i <= 'z'; ++i)
  {
    SNMALLOC_ASSERT(cloned[i - 'a'] == std::string(i, i));
    SNMALLOC_ASSERT(moved[i - 'a'] == std::string(i, i));
  }

  for (auto i = 'A'; i <= 'Z'; ++i)
  {
    cloned.emplace_back(i, i);
    moved.emplace_back(i, i);
  }

  std::swap(cloned, moved);

  for (auto i = 'a'; i <= 'z'; ++i)
  {
    SNMALLOC_ASSERT(cloned[i - 'a'] == std::string(i, i));
    SNMALLOC_ASSERT(moved[i - 'a'] == std::string(i, i));
  }

  for (auto i = 'A'; i <= 'Z'; ++i)
  {
    SNMALLOC_ASSERT(cloned[i - 'A' + ('z' - 'a' + 1)] == std::string(i, i));
    SNMALLOC_ASSERT(moved[i - 'A' + ('z' - 'a' + 1)] == std::string(i, i));
  }
}

int main()
{
  test_rust_global_allocate<true>(sn_rust_alloc);
  test_rust_global_allocate<false>(sn_rust_alloc_zeroed);
  test_allocator_vector<true>();
  test_allocator_vector<false>();
}
