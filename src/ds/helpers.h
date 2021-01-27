#pragma once

#include "bits.h"
#include "flaglock.h"

namespace snmalloc
{
  /*
   * In some use cases we need to run before any of the C++ runtime has been
   * initialised.  This singleton class is designed to not depend on the
   * runtime.
   */
  template<class Object, Object init() noexcept>
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
          obj = init();
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

  template<size_t length, typename T>
  class ModArray
  {
    static constexpr size_t rlength = bits::next_pow2_const(length);
    T array[rlength];

  public:
    constexpr const T& operator[](const size_t i) const
    {
      return array[i & (rlength - 1)];
    }

    constexpr T& operator[](const size_t i)
    {
      return array[i & (rlength - 1)];
    }
  };

  /**
   * Helper class to execute a specified function on destruction.
   */
  template<void f()>
  class OnDestruct
  {
  public:
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
} // namespace snmalloc
