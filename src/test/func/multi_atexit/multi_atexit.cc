#ifdef __linux__
#  include <snmalloc/snmalloc.h>
#  include <stdlib.h>

void do_nothing() {}

extern "C" void * calloc(size_t num, size_t size) {
  return snmalloc::libc::calloc(num, size);
}

extern "C" void free(void * p) {
  return snmalloc::libc::free(p);
}
#endif

int main() {
#ifdef __linux__
  for (int i = 0; i < 8192; ++i)
    atexit(do_nothing);
#endif
}