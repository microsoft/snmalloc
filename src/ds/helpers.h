#pragma once

#include "bits.h"
#include "flaglock.h"

namespace snmalloc
{
  /*
   * In some use cases we need to run before any of the C++ runtime has been
   * initialised.  This singleton class is design to not depend on the runtime.
   */
  template<class Object, Object init() noexcept>
  class Singleton
  {
  public:
    inline static Object& get()
    {
      static std::atomic_flag flag;
      static std::atomic<bool> initialised;
      static Object obj;

      if (!initialised.load(std::memory_order_acquire))
      {
        FlagLock lock(flag);
        if (!initialised)
        {
          obj = init();
          initialised.store(true, std::memory_order_release);
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
   **/
  template<size_t length, typename T>
  class Mod
  {
    static_assert(
      length == bits::next_pow2_const(length), "Must be a power of two.");

  private:
    T value;

  public:
    operator T()
    {
      return static_cast<T>(value & (length - 1));
    }

    T& operator=(const T v)
    {
      value = v;
      return value;
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
}
