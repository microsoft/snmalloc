#include "../override/detours.h"

#include <stdio.h>
#include <stdlib.h>

// This is done in CMake, but could be done here as well
// #pragma comment(linker, "/include:is_snmalloc_detour")

int main()
{
  auto p_old = malloc(16);

  if (p_old != nullptr && !is_snmalloc_detour(p_old))
  {
    printf("Detouring malloc and free failed...\n");
  }

  free(p_old);

  printf("Test passed: Detouring malloc and free succeeded.\n");
  return 0;
}