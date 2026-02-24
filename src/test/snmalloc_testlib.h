#pragma once
/**
 * @file snmalloc_testlib.h
 * @brief Thin test-library header that replaces <snmalloc/snmalloc.h> for
 * tests that can be linked against a pre-compiled static library.
 *
 * Does NOT include snmalloc_core.h — only forward-declares the minimal types
 * needed for the API surface. Tests that need snmalloc_core types (bits::*,
 * sizeclass, Pal, etc.) should include <snmalloc/snmalloc_core.h> themselves
 * before this header.
 *
 * Templates on user-visible params (Conts, align) are declared and backed
 * by explicit instantiations in testlib.cc.
 *
 * Config-templated functions are provided as non-template overloads
 * (distinct from the templates, no ODR conflict) defined in testlib.cc.
 */

#include <errno.h>
#include <snmalloc/ds_core/defines.h>
#include <snmalloc/ds_core/helpers.h>
#include <stddef.h>
#include <stdint.h>

namespace snmalloc
{
  // Forward declarations sufficient for the API surface.
  // address_t: uintptr_t on all non-CHERI platforms.
  using address_t = uintptr_t;

  // ZeroMem has a fixed underlying type (int) so it can be forward-declared
  // here without conflicting with the full definition in pal_consts.h.
  enum ZeroMem : int;
  template<ZeroMem>
  class DefaultConts;
  // Uninit / Zero are usable as template arguments even without the full
  // definition of DefaultConts (only needed inside the allocator itself).
  using Uninit = DefaultConts<ZeroMem(0)>;
  using Zero = DefaultConts<ZeroMem(1)>;

  enum Boundary
  {
    Start,
    End,
    OnePastEnd
  };

  // -- Non-template functions (symbol forced in testlib.cc) ----------------
  void dealloc(void* p);
  void dealloc(void* p, size_t size);
  void dealloc(void* p, size_t size, size_t align);
  void debug_teardown();

  // -- Non-template wrappers for Config-templated functions ----------------
  size_t alloc_size(const void* p);
  size_t remaining_bytes(address_t p);
  bool is_owned(void* p);
  void debug_check_empty(bool* result = nullptr);
  void debug_in_use(size_t count);
  void cleanup_unused();

  // -- Opaque scoped allocator
  // ------------------------------------------------- TestScopedAllocator
  // inherits from ScopedAllocator<> in testlib.cc. Forward-declared here;
  // usable through ScopedAllocHandle.
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

  // -- Constants exposed from allocconfig.h --------------------------------
  size_t max_small_sizeclass_bits();

  // -- PAL/AAL wrappers (avoids needing pal.h in tests) --------------------
  size_t pal_address_bits();
  uint64_t pal_tick();
  void pal_pause();

  // Simple pointer arithmetic (matches snmalloc::pointer_offset for void*).
  // Defined inline so tests that don't include aal/address.h can still use it.
  inline void* pointer_offset(void* base, size_t diff)
  {
    return static_cast<void*>(static_cast<char*>(base) + diff);
  }

  // -- Templates on user-visible params (explicit instantiations in .cc) ---
  template<typename Conts = Uninit, size_t align = 1>
  void* alloc(size_t size);
  extern template void* alloc<Uninit, 1>(size_t);
  extern template void* alloc<Zero, 1>(size_t);

  template<size_t size, typename Conts = Uninit, size_t align = 1>
  void* alloc();

  template<typename Conts = Uninit>
  void* alloc_aligned(size_t align, size_t size);

  template<size_t size>
  void dealloc(void* p);

  // -- external_pointer: declared here, defined + explicitly instantiated in
  // .cc
  template<Boundary location = Start>
  void* external_pointer(void* p);
  extern template void* external_pointer<Start>(void* p);
  extern template void* external_pointer<End>(void* p);
  extern template void* external_pointer<OnePastEnd>(void* p);

  // -- libc namespace ------------------------------------------------------
  namespace libc
  {
    void* malloc(size_t size);
    void free(void* ptr);
    void* calloc(size_t nmemb, size_t size);
    void* realloc(void* ptr, size_t size);
    size_t malloc_usable_size(const void* ptr);
    void* memalign(size_t alignment, size_t size);
    void* aligned_alloc(size_t alignment, size_t size);
    int posix_memalign(void** memptr, size_t alignment, size_t size);
    void* reallocarray(void* ptr, size_t nmemb, size_t size);
    int reallocarr(void* ptr_, size_t nmemb, size_t size);
  } // namespace libc

} // namespace snmalloc

// -- malloc-extensions struct and function ----------------------------------
#include <snmalloc/override/malloc-extensions.h>

// -- override/malloc.cc functions with testlib_ prefix ----------------------
extern "C"
{
  void* testlib_malloc(size_t size);
  void testlib_free(void* ptr);
  void testlib_cfree(void* ptr);
  void* testlib_calloc(size_t nmemb, size_t size);
  size_t testlib_malloc_usable_size(const void* ptr);
  size_t testlib_malloc_good_size(size_t size);
  void* testlib_realloc(void* ptr, size_t size);
  void* testlib_reallocarray(void* ptr, size_t nmemb, size_t size);
  int testlib_reallocarr(void* ptr, size_t nmemb, size_t size);
  void* testlib_memalign(size_t alignment, size_t size);
  void* testlib_aligned_alloc(size_t alignment, size_t size);
  int testlib_posix_memalign(void** memptr, size_t alignment, size_t size);
}
