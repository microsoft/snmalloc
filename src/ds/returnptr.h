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
    void* ptr;

    ReturnPtr(const std::nullptr_t n)
    {
      this->ptr = n;
    }

    inline bool operator==(const ReturnPtr& rhs)
    {
      return this->ptr == rhs.ptr;
    }

    inline bool operator!=(const ReturnPtr& rhs)
    {
      return this->ptr != rhs.ptr;
    }
  };

  /**
   * Construct a ReturnPointer from an arbitrary void pointer.  This is
   * "unsafe" in the sense of Haskell's "unsafe": the type system cannot
   * save us from ourselves, so extra care is required.
   */
  static inline ReturnPtr unsafe_return_ptr(const void* p)
  {
    return *reinterpret_cast<ReturnPtr*>(&p);
  }
} // namespace snmalloc
