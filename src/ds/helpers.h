#pragma once

#include "bits.h"
#include "flaglock.h"

#include <array>
#include <atomic>
#include <type_traits>

namespace snmalloc
{
  /*
   * In some use cases we need to run before any of the C++ runtime has been
   * initialised.  This singleton class is designed to not depend on the
   * runtime.
   */
  template<class Object, void init(Object*) noexcept>
  class Singleton
  {
    inline static std::atomic_flag flag;
    inline static std::atomic<bool> initialised{false};
    inline static Object obj;

  public:
    /**
     * If argument is non-null, then it is assigned the value
     * true, if this is the first call to get.
     * At most one call will be first.
     */
    inline SNMALLOC_SLOW_PATH static Object& get(bool* first = nullptr)
    {
      // If defined should be initially false;
      SNMALLOC_ASSERT(first == nullptr || *first == false);

      if (unlikely(!initialised.load(std::memory_order_acquire)))
      {
        FlagLock lock(flag);
        if (!initialised)
        {
          init(&obj);
          initialised.store(true, std::memory_order_release);
          if (first != nullptr)
            *first = true;
        }
      }
      return obj;
    }
  };

  /**
   * Wrapper for wrapping values.
   *
   * Wraps on read. This allows code to trust the value is in range, even when
   * there is a memory corruption.
   */
  template<size_t length, typename T>
  class Mod
  {
    static_assert(bits::is_pow2(length), "Must be a power of two.");

  private:
    T value = 0;

  public:
    operator T()
    {
      return static_cast<T>(value & (length - 1));
    }

    Mod& operator=(const T v)
    {
      value = v;
      return *this;
    }
  };

#ifdef SNMALLOC_CHECK_CLIENT
  template<size_t length, typename T>
  class ModArray
  {
    /**
     * Align the elements, so that access is cheaper.
     */
    struct alignas(bits::next_pow2_const(sizeof(T))) TWrap
    {
      T v;
    };

    static constexpr size_t rlength = bits::next_pow2_const(length);
    std::array<TWrap, rlength> array;

  public:
    constexpr const T& operator[](const size_t i) const
    {
      return array[i & (rlength - 1)].v;
    }

    constexpr T& operator[](const size_t i)
    {
      return array[i & (rlength - 1)].v;
    }
  };
#else
  template<size_t length, typename T>
  using ModArray = std::array<T, length>;
#endif

  /**
   * Helper class to execute a specified function on destruction.
   */
  template<typename F>
  class OnDestruct
  {
    F f;

  public:
    OnDestruct(F f) : f(f) {}

    ~OnDestruct()
    {
      f();
    }
  };

  /**
   * Non-owning version of std::function. Wraps a reference to a callable object
   * (eg. a lambda) and allows calling it through dynamic dispatch, with no
   * allocation. This is useful in the allocator code paths, where we can't
   * safely use std::function.
   *
   * Inspired by the C++ proposal:
   * http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0792r2.html
   */
  template<typename Fn>
  struct function_ref;
  template<typename R, typename... Args>
  struct function_ref<R(Args...)>
  {
    // The enable_if is used to stop this constructor from shadowing the default
    // copy / move constructors.
    template<
      typename Fn,
      typename =
        std::enable_if_t<!std::is_same_v<std::decay_t<Fn>, function_ref>>>
    function_ref(Fn&& fn)
    {
      data_ = static_cast<void*>(&fn);
      fn_ = execute<Fn>;
    }

    R operator()(Args... args) const
    {
      return fn_(data_, args...);
    }

  private:
    void* data_;
    R (*fn_)(void*, Args...);

    template<typename Fn>
    static R execute(void* p, Args... args)
    {
      return (*static_cast<std::add_pointer_t<Fn>>(p))(args...);
    };
  };

  template<class T, template<typename> typename Ptr>
  void ignore(Ptr<T> t)
  {
    UNUSED(t);
  }

  /**
   * Sometimes we need atomics with trivial initializer.  Unfortunately, this
   * became harder to accomplish in C++20.  Fortunately, our rules for accessing
   * these are at least as strong as those required by C++20's atomic_ref:
   *
   *   * The objects outlive any references to them
   *
   *   * We always access the objects through references (though we'd be allowed
   *     to access them without if we knew there weren't other references)
   *
   *   * We don't access sub-objects at all, much less concurrently through
   *     other references.
   */
  template<typename T>
  class TrivialInitAtomic
  {
    static_assert(
      std::is_trivially_default_constructible_v<T>,
      "TrivialInitAtomic should not attempt to call nontrivial constructors");

#ifdef __cpp_lib_atomic_ref
    using Val = T;
    using Ref = std::atomic_ref<T>;
#else
    using Val = std::atomic<T>;
    using Ref = std::atomic<T>&;
#endif
    Val v;

  public:
    /**
     * Construct a reference to this value; use .load and .store to manipulate
     * the value.
     */
    SNMALLOC_FAST_PATH Ref ref()
    {
#ifdef __cpp_lib_atomic_ref
      return std::atomic_ref<T>(this->v);
#else
      return this->v;
#endif
    }

    SNMALLOC_FAST_PATH T
    load(std::memory_order mo = std::memory_order_seq_cst) noexcept
    {
      return this->ref().load(mo);
    }

    SNMALLOC_FAST_PATH void
    store(T n, std::memory_order mo = std::memory_order_seq_cst) noexcept
    {
      return this->ref().store(n, mo);
    }

    SNMALLOC_FAST_PATH bool compare_exchange_strong(
      T& exp, T des, std::memory_order mo = std::memory_order_seq_cst) noexcept
    {
      return this->ref().compare_exchange_strong(exp, des, mo);
    }

    SNMALLOC_FAST_PATH T
    exchange(T des, std::memory_order mo = std::memory_order_seq_cst) noexcept
    {
      return this->ref().exchange(des, mo);
    }

    template<typename Q>
    SNMALLOC_FAST_PATH
      typename std::enable_if<std::is_integral<Q>::value, Q>::type
      fetch_add(
        Q arg, std::memory_order mo = std::memory_order_seq_cst) noexcept
    {
      return this->ref().fetch_add(arg, mo);
    }
  };

  static_assert(sizeof(TrivialInitAtomic<char>) == sizeof(char));
  static_assert(alignof(TrivialInitAtomic<char>) == alignof(char));
} // namespace snmalloc
