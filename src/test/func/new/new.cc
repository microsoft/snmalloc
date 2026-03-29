#include <snmalloc/pal/pal.h>
#include <test/helpers.h>
#include <test/setup.h>

#include <snmalloc/override/new.cc>

using namespace snmalloc;

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);

  setup();

  snmalloc::debug_check_empty();
  return 0;
}
