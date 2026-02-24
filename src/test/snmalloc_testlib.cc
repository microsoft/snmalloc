/**
 * @file snmalloc_testlib.cc
 * @brief Single translation unit that compiles the full snmalloc allocator
 * and provides symbols for test-library consumers.
 *
 * Built once per flavour (fast / check) into a static library that tests
 * link against, avoiding redundant recompilation of the allocator in every
 * test TU.
 *
 * Three kinds of symbols are provided:
 *
 *  A. Non-templated inline functions (dealloc, debug_teardown, libc::*)
 *     are marked SNMALLOC_USED_FUNCTION in their declarations, so weak/COMDAT
 *     symbols are emitted in every TU that includes them; the linker resolves
 *     test references to the copies in this archive.
 *
 *  B. Templates on user-visible params only (alloc<Conts,align>):
 *     explicit instantiation definitions force emission of strong symbols.
 *
 *  C. Config-templated functions: non-template overloads that call through
 *     to the template<Config> version. These are distinct functions (not
 *     ODR-conflicting with the templates).
 */

#include <snmalloc/snmalloc.h>

namespace snmalloc
{
  // -- B: Explicit instantiation of non-Config templates -------------------

  template void* alloc<Uninit, 1>(size_t);
  template void* alloc<Zero, 1>(size_t);

  // -- C: Non-template wrappers for Config-templated functions -------------

  // Config-templated functions:

  size_t alloc_size(const void* p)
  {
    return alloc_size<Config>(p);
  }

  size_t remaining_bytes(address_t p)
  {
    return remaining_bytes<Config>(p);
  }

  bool is_owned(void* p)
  {
    return is_owned<Config>(p);
  }

  void debug_check_empty(bool* result)
  {
    debug_check_empty<Config>(result);
  }

  void debug_in_use(size_t count)
  {
    debug_in_use<Config>(count);
  }

  void cleanup_unused()
  {
    cleanup_unused<Config>();
  }

  // -- Opaque scoped allocator (inherits from ScopedAllocator<>) -----------

  struct TestScopedAllocator : public ScopedAllocator<>
  {};

  TestScopedAllocator* create_scoped_allocator()
  {
    return new TestScopedAllocator();
  }

  void destroy_scoped_allocator(TestScopedAllocator* p)
  {
    delete p;
  }

  void* scoped_alloc(TestScopedAllocator* a, size_t size)
  {
    return a->alloc->alloc(size);
  }

  // ScopedAllocHandle is declared in snmalloc_testlib.h; define methods here.
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

  ScopedAllocHandle::ScopedAllocHandle() : ptr(create_scoped_allocator()) {}

  ScopedAllocHandle::~ScopedAllocHandle()
  {
    destroy_scoped_allocator(ptr);
  }

  void* ScopedAllocHandle::alloc(size_t size)
  {
    return scoped_alloc(ptr, size);
  }

  void ScopedAllocHandle::dealloc(void* p)
  {
    snmalloc::dealloc(p);
  }

  void ScopedAllocHandle::dealloc(void* p, size_t size)
  {
    snmalloc::dealloc(p, size);
  }

  ScopedAllocHandle* ScopedAllocHandle::operator->()
  {
    return this;
  }

  const ScopedAllocHandle* ScopedAllocHandle::operator->() const
  {
    return this;
  }

  ScopedAllocHandle get_scoped_allocator()
  {
    return {};
  }

  size_t max_small_sizeclass_bits()
  {
    return MAX_SMALL_SIZECLASS_BITS;
  }

  // -- PAL/AAL wrappers ----------------------------------------------------

  size_t pal_address_bits()
  {
    return DefaultPal::address_bits;
  }

  uint64_t pal_tick()
  {
    return DefaultPal::tick();
  }

  void pal_pause()
  {
    Aal::pause();
  }

  // -- external_pointer: 1-param template defined here, explicitly
  // instantiated

  template<Boundary location>
  void* external_pointer(void* p)
  {
    return external_pointer<location, Config>(p);
  }

  template void* external_pointer<Start>(void* p);
  template void* external_pointer<End>(void* p);
  template void* external_pointer<OnePastEnd>(void* p);

} // namespace snmalloc

// -- override/malloc.cc symbols with testlib_ prefix -----------------------
#undef SNMALLOC_NO_REALLOCARRAY
#undef SNMALLOC_NO_REALLOCARR
#define SNMALLOC_NAME_MANGLE(a) testlib_##a
#include <snmalloc/override/malloc.cc>

// malloc-extensions (unprefixed - get_malloc_info_v1)
#include <snmalloc/override/malloc-extensions.cc>
