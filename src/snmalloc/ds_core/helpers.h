#pragma once

#include "bits.h"

#include <array>
#include <atomic>
#include <functional>
#include <string_view>
#include <type_traits>

namespace snmalloc
{
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

#ifdef SNMALLOC_CHECK_CLIENT // TODO is this used/helpful?
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

  /**
   * Helper class for building fatal errors.  Used by `report_fatal_error` to
   * build an on-stack buffer containing the formatted string.
   */
  template<size_t BufferSize>
  class MessageBuilder
  {
    /**
     * The buffer that is used to store the formatted output.
     */
    std::array<char, BufferSize> buffer;

    /**
     * Space in the buffer, excluding a trailing null terminator.
     */
    static constexpr size_t SafeLength = BufferSize - 1;

    /**
     * The insert position within `buffer`.
     */
    size_t insert = 0;

    /**
     * Add argument `i` from the tuple `args` to the output.  This is
     * implemented recursively because the different tuple elements can have
     * different types and so the code for dispatching will depend on the type
     * at the index.  The compiler will lower this to a jump table in optimised
     * builds.
     */
    template<size_t I, typename... Args>
    void add_tuple_arg(size_t i, const std::tuple<Args...>& args)
    {
      if (i == I)
      {
        append(std::get<I>(args));
      }
      else if constexpr (I != 0)
      {
        add_tuple_arg<I - 1>(i, args);
      }
    }

    /**
     * Append a single character into the buffer.  This is the single primitive
     * operation permitted on the buffer and performs bounds checks to ensure
     * that there is space for the character and for a null terminator.
     */
    void append_char(char c)
    {
      if (insert < SafeLength)
      {
        buffer[insert++] = c;
      }
    }

    /**
     * Append a string to the buffer.
     */
    void append(std::string_view sv)
    {
      for (auto c : sv)
      {
        append_char(c);
      }
    }

    /*
     * TODO: This is not quite the right thing we want to check, but it
     * suffices on all currently-supported platforms and CHERI.  We'd rather
     * compare UINTPTR_WIDTH and ULLONG_WIDTH, I think, but those don't
     * exist until C's FP Ext 1 TS (merged into C2x).
     */
#ifdef __CHERI_PURE_CAPABILITY__
    /**
     * Append an intptr_t to the buffer as a hex string
     */
    void append(intptr_t v)
    {
      append(reinterpret_cast<void*>(v));
    }

    /**
     * Append a uintptr_t to the buffer as a hex string
     */
    void append(uintptr_t v)
    {
      append(reinterpret_cast<void*>(v));
    }
#endif

    /**
     * Append a raw pointer to the buffer as a hex string.
     */
    void append(void* ptr)
    {
      append(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ptr)));
      // TODO: CHERI bits.
    }

    /**
     * Append a signed integer to the buffer, as a decimal string.
     */
    void append(long long s)
    {
      if (s < 0)
      {
        append_char('-');
        s = 0 - s;
      }
      std::array<char, 20> buf{{0}};
      const char digits[] = "0123456789";
      for (long i = static_cast<long>(buf.size() - 1); i >= 0; i--)
      {
        buf[static_cast<size_t>(i)] = digits[s % 10];
        s /= 10;
      }
      bool skipZero = true;
      for (auto c : buf)
      {
        if (skipZero && (c == '0'))
        {
          continue;
        }
        skipZero = false;
        append_char(c);
      }
      if (skipZero)
      {
        append_char('0');
      }
    }

    /**
     * Append a size to the buffer, as a hex string.
     */
    void append(unsigned long long s)
    {
      append_char('0');
      append_char('x');
      std::array<char, 16> buf{{0}};
      const char hexdigits[] = "0123456789abcdef";
      // Length of string including null terminator
      static_assert(sizeof(hexdigits) == 0x11);
      for (long i = static_cast<long>(buf.size() - 1); i >= 0; i--)
      {
        buf[static_cast<size_t>(i)] = hexdigits[s & 0xf];
        s >>= 4;
      }
      bool skipZero = true;
      for (auto c : buf)
      {
        if (skipZero && (c == '0'))
        {
          continue;
        }
        skipZero = false;
        append_char(c);
      }
      if (skipZero)
      {
        append_char('0');
      }
    }

    /**
     * Overload to force `long` to be promoted to `long long`.
     */
    void append(long x)
    {
      append(static_cast<long long>(x));
    }

    /**
     * Overload to force `unsigned long` to be promoted to `unsigned long long`.
     */
    void append(unsigned long x)
    {
      append(static_cast<unsigned long long>(x));
    }

    /**
     * Overload to force `int` to be promoted to `long long`.
     */
    void append(int x)
    {
      append(static_cast<long long>(x));
    }

    /**
     * Overload to force `unsigned int` to be promoted to `unsigned long long`.
     */
    void append(unsigned int x)
    {
      append(static_cast<unsigned long long>(x));
    }

  public:
    /**
     * Constructor.  Takes a format string and the arguments to output.
     */
    template<typename... Args>
    SNMALLOC_FAST_PATH MessageBuilder(const char* fmt, Args... args)
    {
      buffer[SafeLength] = 0;
      size_t arg = 0;
      auto args_tuple = std::forward_as_tuple(args...);
      for (const char* s = fmt; *s != 0; ++s)
      {
        if (s[0] == '{' && s[1] == '}')
        {
          add_tuple_arg<sizeof...(Args) - 1>(arg++, args_tuple);
          ++s;
        }
        else
        {
          append_char(*s);
        }
      }
      append_char('\0');
    }

    /**
     * Constructor for trivial format strings (no arguments).  This exists to
     * allow `MessageBuilder` to be used with macros without special casing
     * the single-argument version.
     */
    SNMALLOC_FAST_PATH MessageBuilder(const char* fmt)
    {
      buffer[SafeLength] = 0;
      for (const char* s = fmt; *s != 0; ++s)
      {
        append_char(*s);
      }
      append_char('\0');
    }

    /**
     * Return the error buffer.
     */
    const char* get_message()
    {
      return buffer.data();
    }
  };

  /**
   * Convenience type that has no fields / methods.
   */
  struct Empty
  {};

} // namespace snmalloc
