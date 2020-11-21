#pragma once

namespace snmalloc
{
  /**
   * A convenience type that we can use to annotate internal functions
   * which return pointers that are headed out to the application or
   * which have come back.
   */
  class ReturnPtr
  {
  public:
    void* unsafe_return_ptr;

    ReturnPtr(const std::nullptr_t n)
    {
      this->unsafe_return_ptr = n;
    }

    inline bool operator==(const ReturnPtr& rhs) const
    {
      return this->unsafe_return_ptr == rhs.unsafe_return_ptr;
    }

    inline bool operator!=(const ReturnPtr& rhs) const
    {
      return this->unsafe_return_ptr != rhs.unsafe_return_ptr;
    }
  };

  /**
   * A convenience type used to indicate a pointer suitable for inclusion on a
   * free list.  The pointer itself is ready to be given to a client (that is,
   * it has restricted authority on StrictProvenance architectures), but the
   * contents of the memory are possibly unsafe to disclose.
   *
   * Obtain non-null `FreePtr`s using the AAL's `ptrauth_bound` method.
   */
  template<typename T>
  class FreePtr
  {
  public:
    T* unsafe_free_ptr;

    FreePtr(const std::nullptr_t n) : unsafe_free_ptr(n) {}

    template<typename U>
    explicit FreePtr(FreePtr<U> fp)
    : unsafe_free_ptr(reinterpret_cast<T*>(fp.unsafe_free_ptr))
    {}

    inline bool operator==(const FreePtr& rhs) const
    {
      return this->unsafe_free_ptr == rhs.unsafe_free_ptr;
    }

    inline bool operator!=(const FreePtr& rhs) const
    {
      return this->unsafe_free_ptr != rhs.unsafe_free_ptr;
    }
  };

  /**
   * A convenience type used to indicate that a pointer has elevated authority
   * and requires some special care.
   *
   * Most non-NULL `AuthPtr`s in the Alloc-ator proper come from amplification;
   * see the largealloc's `ptrauth_amplify` method.
   */
  template<typename T>
  class AuthPtr
  {
  public:
    T* unsafe_auth_ptr;

    AuthPtr() : unsafe_auth_ptr(nullptr) {}

    AuthPtr(const std::nullptr_t n) : unsafe_auth_ptr(n) {}

    inline bool operator==(const AuthPtr& rhs) const
    {
      return this->unsafe_auth_ptr == rhs.unsafe_auth_ptr;
    }

    inline bool operator!=(const AuthPtr& rhs) const
    {
      return this->unsafe_auth_ptr != rhs.unsafe_auth_ptr;
    }

    inline bool operator<(const AuthPtr& rhs) const
    {
      return this->unsafe_auth_ptr < rhs.unsafe_auth_ptr;
    }
  };

  /**
   * While it's always safe to view a pointer as carrying its authority, we want
   * to be explicit when we're doing so, so this isn't a constructor for the
   * `AuthPtr` class, which would be an implicit conversion.
   */
  template<typename T = void>
  SNMALLOC_FAST_PATH static AuthPtr<T> mk_authptr(void* p)
  {
    return *reinterpret_cast<AuthPtr<T>*>(&p);
  }

  /**
   * Occasionally we want to treat an `AuthPtr` as a `FreePtr` with the
   * knowledge that it isn't actually headed out to the user.
   */
  template<typename T, typename U = T>
  SNMALLOC_FAST_PATH static FreePtr<T> unsafe_mk_freeptr(AuthPtr<U> p)
  {
    return *reinterpret_cast<FreePtr<T>*>(&p.unsafe_auth_ptr);
  }

  template<typename T>
  SNMALLOC_FAST_PATH static FreePtr<T> unsafe_as_freeptr(ReturnPtr p)
  {
    return *reinterpret_cast<FreePtr<T>*>(&p.unsafe_return_ptr);
  }

  /**
   * `ReturnPtr`s are `FreePtr`s that have had their contents sanitized; there
   * is no great way to explain that to the type system, so carefully use this
   * function after that sanitization.
   */
  template<typename T>
  SNMALLOC_FAST_PATH static ReturnPtr unsafe_mk_returnptr(FreePtr<T> p)
  {
    return *reinterpret_cast<ReturnPtr*>(&p.unsafe_free_ptr);
  }

  /**
   * `ReturnPtr`s are sometimes given back to us as `void *`.
   */
  SNMALLOC_FAST_PATH static ReturnPtr unsafe_as_returnptr(void* p)
  {
    return *reinterpret_cast<ReturnPtr*>(&p);
  }

} // namespace snmalloc
