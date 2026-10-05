/**
 * Windows fiber-local storage (FLS) callbacks for the main thread run inside
 * ExitProcess, after the CRT has run atexit handlers and static destructors.
 * Rust's thread-local destructors are implemented this way, so this test
 * checks that memory allocated by snmalloc can still be accessed and freed
 * from such a callback.
 */

#include <stdio.h>
#include <test/snmalloc_testlib.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

struct Object
{
  volatile size_t count;
};

void WINAPI fls_callback(void* p)
{
  if (p == nullptr)
    return;

  auto* o = static_cast<Object*>(p);
  // Mirrors dropping a Rust Arc: an access to the object, then a free.
  o->count = o->count - 1;
  snmalloc::dealloc(o);
}

int main()
{
  DWORD key = FlsAlloc(&fls_callback);
  if (key == FLS_OUT_OF_INDEXES)
  {
    printf("FlsAlloc failed\n");
    return 1;
  }

  auto* o = static_cast<Object*>(snmalloc::alloc(sizeof(Object)));
  o->count = 1;

  if (!FlsSetValue(key, o))
  {
    printf("FlsSetValue failed\n");
    return 1;
  }

  // Returning from main runs the CRT exit path, then ExitProcess, which runs
  // fls_callback for this thread.
  return 0;
}
#else
int main()
{
  return 0;
}
#endif
