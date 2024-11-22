#if defined(SNMALLOC_CI_BUILD)
#  include <snmalloc/pal/pal.h>
#  if defined(WIN32)
#    include <iostream>
#    include <signal.h>
#    include <snmalloc/ds_core/ds_Core.h>
#    include <stdlib.h>
// Has to come after the PAL.
#    include <DbgHelp.h>
#    pragma comment(lib, "dbghelp.lib")

void print_stack_trace()
{
  DWORD error;
  HANDLE hProcess = GetCurrentProcess();

  char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
  PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;

  pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  pSymbol->MaxNameLen = MAX_SYM_NAME;

  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);

  if (!SymInitialize(hProcess, NULL, TRUE))
  {
    // SymInitialize failed
    error = GetLastError();
    printf("SymInitialize returned error : %lu\n", error);
    return;
  }

  void* stack[1024];
  DWORD count = CaptureStackBackTrace(0, 1024, stack, NULL);
  IMAGEHLP_LINE64 line;
  line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

  for (int i = 0; count > 0; count--, i++)
  {
    DWORD64 dwDisplacement = 0;
    DWORD64 dwAddress = (DWORD64)stack[i];

    if (SymFromAddr(hProcess, dwAddress, &dwDisplacement, pSymbol))
    {
      DWORD dwDisplacement2 = 0;
      if (SymGetLineFromAddr64(hProcess, dwAddress, &dwDisplacement2, &line))
      {
        std::cerr << "Frame: " << pSymbol->Name << " (" << line.FileName << ": "
                  << line.LineNumber << ")" << std::endl;
      }
      else
      {
        std::cerr << "Frame: " << pSymbol->Name << std::endl;
      }
    }
    else
    {
      error = GetLastError();
      std::cerr << "SymFromAddr returned error : " << error << std::endl;
    }
  }
}

void _cdecl error(int signal)
{
  snmalloc::UNUSED(signal);
  snmalloc::DefaultPal::message("*****ABORT******");

  print_stack_trace();

  _exit(1);
}

LONG WINAPI VectoredHandler(struct _EXCEPTION_POINTERS* ExceptionInfo)
{
  snmalloc::UNUSED(ExceptionInfo);

  snmalloc::DefaultPal::message("*****UNHANDLED EXCEPTION******");

  print_stack_trace();

  _exit(1);
}

void setup()
{
  // Disable abort dialog box in CI builds.
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG);
  signal(SIGABRT, error);

  // If we have an unhandled exception print a stack trace.
  SetUnhandledExceptionFilter(VectoredHandler);

  // Disable OS level dialog boxes during CI.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}
#  else
#    include <signal.h>

void error_handle(int signal)
{
  snmalloc::UNUSED(signal);
  snmalloc::error("Seg Fault");
  _exit(1);
}

void setup()
{
  signal(SIGSEGV, error_handle);
}
#  endif
#else
void setup() {}
#endif
