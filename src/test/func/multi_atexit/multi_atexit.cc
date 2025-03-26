#ifdef __linux__
#  include <snmalloc/snmalloc.h>
#  include <stdlib.h>

void do_nothing() {}
#define SNMALLOC_NAME_MANGLE(X) X
#include "snmalloc/override/malloc.cc"

#endif

int main() {
#ifdef __linux__
  for (int i = 0; i < 8192; ++i)
    atexit(do_nothing);
#endif
}