/**
 * Checks when a DLL that uses snmalloc returns its memory to the OS.
 *
 * When the DLL is unloaded with FreeLibrary while the process continues to
 * run, the memory it reserved must be released.
 *
 * When the process exits, the memory must not be released, as code run later
 * by the loader may still access allocations.  Here, a second DLL that was
 * loaded first, so is detached last, accesses an allocation from the first
 * DLL in its process-exit detach notification.
 */

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

using AllocFn = void* (*)(size_t);
using DeallocFn = void (*)(void*);
using SetObjectFn = void (*)(size_t*);

template<typename Fn>
Fn get_function(HMODULE module, const char* name)
{
  FARPROC proc = GetProcAddress(module, name);
  if (proc == nullptr)
  {
    printf("GetProcAddress(%s) failed\n", name);
    exit(1);
  }
  return reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(proc));
}

HMODULE load(const char* name)
{
  HMODULE module = LoadLibraryA(name);
  if (module == nullptr)
  {
    printf("LoadLibrary(%s) failed: %lu\n", name, GetLastError());
    exit(1);
  }
  return module;
}

DWORD state(void* p)
{
  MEMORY_BASIC_INFORMATION info;
  if (VirtualQuery(p, &info, sizeof(info)) == 0)
  {
    printf("VirtualQuery failed: %lu\n", GetLastError());
    exit(1);
  }
  return info.State;
}

constexpr const char* alloc_dll = "snmalloc-dll-teardown-alloc.dll";
constexpr const char* touch_dll = "snmalloc-dll-teardown-touch.dll";

int main()
{
  // The touch DLL is loaded first, so it is detached after the alloc DLL
  // during process exit.
  HMODULE touch = load(touch_dll);

  // FreeLibrary while the process continues to run releases the memory.
  {
    HMODULE alloc = load(alloc_dll);
    auto p = get_function<AllocFn>(alloc, "dll_teardown_alloc")(sizeof(size_t));
    if (state(p) != MEM_COMMIT)
    {
      printf("Allocation is not committed\n");
      return 1;
    }
    get_function<DeallocFn>(alloc, "dll_teardown_dealloc")(p);

    FreeLibrary(alloc);
    if (GetModuleHandleA(alloc_dll) != nullptr)
    {
      // Some runtimes, such as MinGW's, pin DLLs that use thread_local
      // destructors, so FreeLibrary does not unload them.
      printf("DLL was not unloaded; skipping release check\n");
    }
    else if (state(p) != MEM_FREE)
    {
      printf("Memory was not released by FreeLibrary\n");
      return 1;
    }
  }

  // Process exit must not release the memory, as the touch DLL accesses it
  // after the alloc DLL has been detached.
  HMODULE alloc = load(alloc_dll);
  auto p = static_cast<size_t*>(
    get_function<AllocFn>(alloc, "dll_teardown_alloc")(sizeof(size_t)));
  *p = 0;
  get_function<SetObjectFn>(touch, "dll_teardown_set_object")(p);

  printf("Exiting\n");
  return 0;
}
#else
int main()
{
  return 0;
}
#endif
