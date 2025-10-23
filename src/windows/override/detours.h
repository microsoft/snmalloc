#pragma once

class SnmallocDetour
{
public:
  SnmallocDetour();
  ~SnmallocDetour();
};

// Used to check that a pointer is from the snmalloc detour.
extern "C" __declspec(dllexport) bool is_snmalloc_detour(void* ptr);