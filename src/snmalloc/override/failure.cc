#include <cerrno>
#include <cstddef>
#include <new>

namespace snmalloc
{
  void* alloc_nothrow(size_t size);
  void* alloc_throw(size_t size);

#ifdef SNMALLOC_NOEXC
  // If we are compiling without exceptions, we cannot usefully use
  // new_handlers to retry allocations, so we just ignore them and
  // return nullptr on failure.
  void* failure_throw(std::size_t)
  {
    // If we are here, then the allocation failed.
    // Set errno to ENOMEM, as per the C standard.
    errno = ENOMEM;
    // Return nullptr on failure.
    return nullptr;
  }

  void* failure_nothrow(std::size_t)
  {
    // If we are here, then the allocation failed.
    // Set errno to ENOMEM, as per the C standard.
    errno = ENOMEM;
    // Return nullptr on failure.
    return nullptr;
  }
#else
  void* failure_throw(std::size_t size)
  {
    auto new_handler = std::get_new_handler();
    if (new_handler != nullptr)
    {
      // Call the new handler, which may throw an exception.
      new_handler();
      // Retry now new_handler has been called.
      // I dislike the unbounded retrying here, but that seems to be what
      // other implementations do.
      return alloc_throw(size);
    }

    // Throw std::bad_alloc on failure.
    throw std::bad_alloc();
  }

  void* failure_nothrow(std::size_t size)
  {
    auto new_handler = std::get_new_handler();
    if (new_handler != nullptr)
    {
      try
      {
        // Call the new handler, which may throw an exception.
        new_handler();
      }
      catch (...)
      {
        // If the new handler throws, we just return nullptr.
        return nullptr;
      }
      // Retry now new_handler has been called.
      return alloc_nothrow(size);
    }
    // If we are here, then the allocation failed.
    // Set errno to ENOMEM, as per the C standard.
    errno = ENOMEM;

    // Return nullptr on failure.
    return nullptr;
  }
#endif
}

