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
  template<ZeroMem zero_mem>
  void* alloc(size_t size)
  {
    if constexpr (zero_mem == ZeroMem::YesZero)
    {
      return alloc<Zero>(size);
    }
    else
    {
      return alloc<Uninit>(size);
    }
  }

  template void* alloc<ZeroMem::YesZero>(size_t size);
  template void* alloc<ZeroMem::NoZero>(size_t size);

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

  // -- Force emission of inline functions for MSVC (where USED_FUNCTION is
  // empty). Taking the address of each inline function forces the compiler to
  // emit a definition in this TU, making it available to testlib consumers.

  SNMALLOC_USED_FUNCTION static const volatile void* force_emit_global[] = {
    reinterpret_cast<volatile void*>(
      static_cast<void (*)(void*)>(&dealloc)),
    reinterpret_cast<volatile void*>(
      static_cast<void (*)(void*, size_t)>(&dealloc)),
    reinterpret_cast<volatile void*>(
      static_cast<void (*)(void*, size_t, size_t)>(&dealloc)),
    reinterpret_cast<volatile void*>(&debug_teardown),
  };

  namespace libc
  {
    SNMALLOC_USED_FUNCTION static const volatile void* force_emit_libc[] = {
      reinterpret_cast<volatile void*>(&__malloc_start_pointer),
      reinterpret_cast<volatile void*>(&__malloc_last_byte_pointer),
      reinterpret_cast<volatile void*>(&__malloc_end_pointer),
      reinterpret_cast<volatile void*>(&malloc),
      reinterpret_cast<volatile void*>(&free),
      reinterpret_cast<volatile void*>(
        static_cast<void* (*)(size_t, size_t)>(&calloc)),
      reinterpret_cast<volatile void*>(
        static_cast<void* (*)(void*, size_t)>(&realloc)),
      reinterpret_cast<volatile void*>(&malloc_usable_size),
      reinterpret_cast<volatile void*>(&memalign),
      reinterpret_cast<volatile void*>(&aligned_alloc),
      reinterpret_cast<volatile void*>(&posix_memalign),
      reinterpret_cast<volatile void*>(&malloc_small),
      reinterpret_cast<volatile void*>(&malloc_small_zero),
    };
  } // namespace libc
} // namespace snmalloc

// -- override/malloc.cc symbols with testlib_ prefix -----------------------
#undef SNMALLOC_NO_REALLOCARRAY
#undef SNMALLOC_NO_REALLOCARR
#define SNMALLOC_NAME_MANGLE(a) testlib_##a
#include <snmalloc/override/malloc.cc>

// malloc-extensions (unprefixed - get_malloc_info_v1)
#include <snmalloc/override/malloc-extensions.cc>
