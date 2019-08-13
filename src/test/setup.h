#if defined(WIN32) && defined(SNMALLOC_CI_BUILD)
#  include <ds/bits.h>
#  include <signal.h>
#  include <stdlib.h>
void _cdecl error(int signal)
{
  UNUSED(signal);
  puts("*****ABORT******");
  _exit(1);
}
void setup()
{
  // Disable abort dialog box in CI builds.
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG);
  signal(SIGABRT, error);
}
#else
void setup() {}
#endif
