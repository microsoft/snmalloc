/**
 * DLL that accesses an object during its process-exit detach notification,
 * for the dll_teardown test.  The object is allocated by a different DLL that
 * uses snmalloc and was loaded after this one, so it is detached before this
 * one.
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{
  volatile size_t* object = nullptr;
}

extern "C" __declspec(dllexport) void dll_teardown_set_object(size_t* p)
{
  object = p;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved)
{
  // A non-null reserved argument indicates process exit.
  if (
    (reason == DLL_PROCESS_DETACH) && (reserved != nullptr) &&
    (object != nullptr))
  {
    // The loader may catch exceptions raised by DllMain, so check that the
    // memory is still committed rather than relying on an access violation.
    MEMORY_BASIC_INFORMATION info;
    if (
      (VirtualQuery(const_cast<size_t*>(object), &info, sizeof(info)) == 0) ||
      (info.State != MEM_COMMIT))
    {
      const char msg[] = "Memory was released before process exit\n";
      DWORD written;
      WriteFile(
        GetStdHandle(STD_ERROR_HANDLE),
        msg,
        sizeof(msg) - 1,
        &written,
        nullptr);
      TerminateProcess(GetCurrentProcess(), 1);
    }
    *object = *object + 1;
  }
  return TRUE;
}
