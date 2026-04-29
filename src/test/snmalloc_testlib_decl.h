#pragma once
/**
 * @file snmalloc_testlib_decl.h
 * @brief ScopedAllocHandle and related declarations.
 *
 * This header is safe to include after <snmalloc/snmalloc.h> (no conflicting
 * type definitions). It provides the single definition of ScopedAllocHandle
 * shared by both snmalloc_testlib.h and snmalloc_testlib.cc.
 */

#include <stddef.h>

namespace snmalloc
{
  // TestScopedAllocator inherits from ScopedAllocator<> in testlib.cc.
  // Forward-declared here; usable through ScopedAllocHandle.
  struct TestScopedAllocator;
  TestScopedAllocator* create_scoped_allocator();
  void destroy_scoped_allocator(TestScopedAllocator*);
  void* scoped_alloc(TestScopedAllocator*, size_t size);

  struct ScopedAllocHandle
  {
    TestScopedAllocator* ptr;

    ScopedAllocHandle();
    ~ScopedAllocHandle();
    ScopedAllocHandle(const ScopedAllocHandle&) = delete;
    ScopedAllocHandle& operator=(const ScopedAllocHandle&) = delete;
    void* alloc(size_t size);

    void dealloc(void* p);
    void dealloc(void* p, size_t size);

    ScopedAllocHandle* operator->();
    const ScopedAllocHandle* operator->() const;
  };

  ScopedAllocHandle get_scoped_allocator();
} // namespace snmalloc
