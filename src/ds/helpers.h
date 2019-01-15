#pragma once

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
}
