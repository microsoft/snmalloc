#pragma once

#include "../ds/mpmcstack.h"
#include "allocconfig.h"

namespace snmalloc
{
  enum SlabKind
  {
    Fresh = 0,
    Large,
    Medium,
    Super
  };

  class Baseslab
  {
  protected:
    SlabKind kind;

  public:
    SlabKind get_kind()
    {
      return kind;
    }
  };
}
