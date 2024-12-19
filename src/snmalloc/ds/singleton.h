#pragma once

#include "../ds_core/ds_core.h"
#include "flaglock.h"
#include "snmalloc/stl/atomic.h"
#include "snmalloc/stl/type_traits.h"

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
    inline static FlagWord flag;
    inline static stl::Atomic<bool> initialised{false};
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

      if (SNMALLOC_UNLIKELY(!initialised.load(stl::memory_order_acquire)))
      {
        with(flag, [&]() {
          if (!initialised)
          {
            init(&obj);
            initialised.store(true, stl::memory_order_release);
            if (first != nullptr)
              *first = true;
          }
        });
      }
      return obj;
    }
  };

} // namespace snmalloc
