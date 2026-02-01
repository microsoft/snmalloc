/*
 * This file provides a Windows-specific overriding of the malloc,
 * free, calloc, realloc, and msize functions using the Detours library.
 */
#ifdef _DEBUG
#  include <crtdbg.h>
#endif

#include "detours.h"

#include <new>
#include <stdlib.h>

// Symbols for the original malloc, free, calloc, realloc, and msize functions
auto* original_malloc = malloc;
auto* original_calloc = calloc;
auto* original_realloc = realloc;
auto* original_free = free;
auto* original_msize = _msize;

void* (*original_new)(size_t) = operator new;
void* (*original_new2)(size_t, const std::nothrow_t&) = operator new;

void* (*original_new_array)(size_t) = operator new[];
void* (*original_new_array2)(size_t, const std::nothrow_t&) = operator new[];

void (*original_delete)(void*) = operator delete;
void (*original_delete2)(void*, size_t) = operator delete;
void (*original_delete3)(void*, const std::nothrow_t&) = operator delete;

void (*original_delete_array)(void*) = operator delete[];
void (*original_delete_array2)(void*, size_t) = operator delete[];

#include <snmalloc/snmalloc_core.h>
// Provides the global configuration for the snmalloc implementation.
#include <snmalloc/backend/globalconfig.h>

#define SNMALLOC_PROVIDE_OWN_CONFIG

namespace snmalloc
{
  class WindowsHeapAsSecondaryAllocator
  {
  public:
    // This flag is used to turn off checks on fast paths if the secondary
    // allocator does not own the memory at all.
    static constexpr inline bool pass_through = false;

    SNMALLOC_FAST_PATH
    static void initialize() {}

    // We always use snmalloc for allocation.
    template<class SizeAlign>
    SNMALLOC_FAST_PATH static void* allocate(SizeAlign&&)
    {
      return nullptr;
    }

    // If the memory was not deallocated by snmalloc, then try the
    // original free.
    SNMALLOC_FAST_PATH
    static void deallocate(void* pointer)
    {
      if (pointer == nullptr)
        return;

      original_free(pointer);
    }

    SNMALLOC_FAST_PATH
    static size_t alloc_size(const void* p)
    {
      return original_msize(const_cast<void*>(p));
    }
  };

  // Root failed deallocations and msize requests to the Windows heap.
  using Config = snmalloc::StandardConfigClientMeta<
    NoClientMetaDataProvider,
    WindowsHeapAsSecondaryAllocator>;
  using Alloc = snmalloc::Allocator<Config>;
} // namespace snmalloc

#define SNMALLOC_STATIC_LIBRARY_PREFIX snmalloc_
#include "detours/detours.h"

#include <snmalloc/override/malloc.cc>
#include <stdio.h>
#include <windows.h>

// This name is not provided by malloc.cc above, so we define it here.
size_t snmalloc_msize(void* ptr)
{
  // Call the snmalloc function to get the allocation size.
  // This is not accurate as it rounds up, whereas the original msize
  // function returns the exact size of the allocation.
  return snmalloc::alloc_size(ptr);
}

SnmallocDetour::SnmallocDetour()
{
  // Initilialize snmalloc.
  snmalloc_free(snmalloc_malloc(1));

  DetourTransactionBegin();
  DetourAttach(&(PVOID&)original_free, snmalloc_free);
  DetourAttach(&(PVOID&)original_delete, snmalloc_free);
  DetourAttach(&(PVOID&)original_delete2, snmalloc_free);
  DetourAttach(&(PVOID&)original_delete3, snmalloc_free);
  DetourAttach(&(PVOID&)original_delete_array, snmalloc_free);
  DetourAttach(&(PVOID&)original_delete_array2, snmalloc_free);
  DetourAttach(&(PVOID&)original_malloc, snmalloc_malloc);
  DetourAttach(&(PVOID&)original_calloc, snmalloc_calloc);
  DetourAttach(&(PVOID&)original_realloc, snmalloc_realloc);
  DetourAttach(&(PVOID&)original_msize, snmalloc_msize);
  DetourAttach(&(PVOID&)original_new, snmalloc_malloc);
  DetourAttach(&(PVOID&)original_new2, snmalloc_malloc);
  DetourAttach(&(PVOID&)original_new_array, snmalloc_malloc);
  DetourAttach(&(PVOID&)original_new_array2, snmalloc_malloc);

  DetourTransactionCommit();
}

SnmallocDetour::~SnmallocDetour()
{
  // Detours performs allocation so during this some data structures will
  // be allocated with snmalloc.  These cannot be handled by the Windows heap
  // so leave snmalloc::free in place to handle these allocations.

  DetourTransactionBegin();
  DetourDetach(&(PVOID&)original_calloc, snmalloc_calloc);
  DetourDetach(&(PVOID&)original_realloc, snmalloc_realloc);
  DetourDetach(&(PVOID&)original_malloc, snmalloc_malloc);
  DetourDetach(&(PVOID&)original_msize, snmalloc_msize);
  DetourDetach(&(PVOID&)original_new, snmalloc_malloc);
  DetourDetach(&(PVOID&)original_new2, snmalloc_malloc);
  DetourDetach(&(PVOID&)original_new_array, snmalloc_malloc);
  DetourDetach(&(PVOID&)original_new_array2, snmalloc_malloc);
  DetourTransactionCommit();

  // This transaction's allocation will come from the Windows heap, so it is
  // safe to use the Windows heap's free during teardown.
  DetourTransactionBegin();
  DetourDetach(&(PVOID&)original_free, snmalloc_free);
  DetourDetach(&(PVOID&)original_delete, snmalloc_free);
  DetourAttach(&(PVOID&)original_delete2, snmalloc_free);
  DetourDetach(&(PVOID&)original_delete3, snmalloc_free);
  DetourDetach(&(PVOID&)original_delete_array, snmalloc_free);
  DetourDetach(&(PVOID&)original_delete_array2, snmalloc_free);
  DetourTransactionCommit();
}

extern "C" bool is_snmalloc_detour(void* ptr)
{
  return snmalloc::is_owned(ptr);
}