/**
 * This file demonstrates how the snmalloc library could be implemented to
 * provide a miracle pointer like feature.  This is not a hardened
 * implementation and is purely for illustrative purposes.
 *
 * Do not use as is.
 */

#ifdef SNMALLOC_THREAD_SANITIZER_ENABLED
int main()
{
  return 0;
}
#else

#  include "test/setup.h"

#  include <atomic>
#  include <iostream>
#  include <memory>
#  include <snmalloc/backend/globalconfig.h>
#  include <snmalloc/snmalloc_core.h>

namespace snmalloc
{
  // Instantiate the allocator with a client meta data provider that uses an
  // atomic size_t to store the reference count.
  using Config = snmalloc::StandardConfigClientMeta<
    ArrayClientMetaDataProvider<std::atomic<size_t>>>;
}

#  define SNMALLOC_PROVIDE_OWN_CONFIG
#  include <snmalloc/snmalloc.h>

SNMALLOC_SLOW_PATH void error(std::string msg)
{
  std::cout << msg << std::endl;
  abort();
}

SNMALLOC_FAST_PATH_INLINE void check(bool b, std::string msg)
{
  if (SNMALLOC_UNLIKELY(!b))
    error(msg);
}

namespace snmalloc::miracle
{
  // snmalloc meta-data representation
  //   * 2n + 1:  Represents an object that has not been deallocated with n
  //              additional references to it
  //   * 2n    :  Represents a deallocated object that
  //              has n additional references to it

  inline void* malloc(size_t size)
  {
    auto p = snmalloc::libc::malloc(size);
    if (SNMALLOC_UNLIKELY(p == nullptr))
      return nullptr;

    snmalloc::get_client_meta_data(p) = 1;
    return p;
  }

  inline void free(void* ptr)
  {
    if (ptr == nullptr)
      return;

    // TODO could build a check into this that it is the start of the object?
    auto previous = snmalloc::get_client_meta_data(ptr).fetch_add((size_t)-1);

    if (SNMALLOC_LIKELY(previous == 1))
    {
      std::cout << "Freeing " << ptr << std::endl;
      snmalloc::libc::free(ptr);
      return;
    }

    check((previous & 1) == 1, "Double free detected");

    // We have additional references to this object.
    // We should not free it.
    // TOOD this assumes this is not an internal pointer.
    memset(ptr, 0, snmalloc::libc::malloc_usable_size(ptr));
  }

  inline void acquire(void* p)
  {
    auto previous = snmalloc::get_client_meta_data(p).fetch_add((size_t)2);

    // Can we take new pointers to a deallocated object?
    check((previous & 1) == 1, "Acquiring a deallocated object");
  }

  inline void release(void* p)
  {
    auto previous = snmalloc::get_client_meta_data(p).fetch_add((size_t)-2);

    if (previous > 2)
      return;

    check(previous == 2, "Releasing an object with insufficient references");

    std::cout << "Freeing from release " << p << std::endl;
    snmalloc::libc::free(p);
  }

  /**
   * This class can be used to replace a raw pointer. It will automatically use
   * the underlying backup reference counting design from the miracle pointer
   * docs.
   */
  template<typename T>
  class raw_ptr
  {
    T* p;

  public:
    raw_ptr() : p(nullptr) {}

    raw_ptr(T* p) : p(p)
    {
      snmalloc::miracle::acquire(p);
    }

    T& operator*()
    {
      return *p;
    }

    ~raw_ptr()
    {
      if (p == nullptr)
        return;
      snmalloc::miracle::release(p);
    }

    raw_ptr(const raw_ptr& rp) : p(rp.p)
    {
      snmalloc::miracle::acquire(p);
    }

    raw_ptr& operator=(const raw_ptr& other)
    {
      p = other.p;
      snmalloc::miracle::acquire(other.p);
      return *this;
    }

    raw_ptr(raw_ptr&& other) : p(other.p)
    {
      other.p = nullptr;
    }

    raw_ptr& operator=(raw_ptr&& other)
    {
      p = other.p;
      other.p = nullptr;
      return *this;
    }
  };
} // namespace snmalloc::miracle

/**
 * Overload new and delete to use the "miracle pointer" implementation.
 */
void* operator new(size_t size)
{
  return snmalloc::miracle::malloc(size);
}

void operator delete(void* p)
{
  snmalloc::miracle::free(p);
}

void operator delete(void* p, size_t)
{
  snmalloc::miracle::free(p);
}

int main()
{
  snmalloc::miracle::raw_ptr<int> p;
  {
    auto up1 = std::make_unique<int>(41);
    auto up = std::make_unique<int>(42);
    auto up2 = std::make_unique<int>(40);
    auto up3 = std::make_unique<int>(39);
    p = up.get();
    check(*p == 42, "Failed to set p");
  }
  // Still safe to access here.  The unique_ptr has been destroyed, but the
  // raw_ptr has kept the memory live.
  // Current implementation zeros the memory when the unique_ptr is destroyed.
  check(*p == 0, "Failed to keep memory live");
  return 0;
}
#endif
