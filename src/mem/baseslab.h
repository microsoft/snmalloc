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
    Super,
    /**
     * If the decommit policy is lazy, slabs are moved to this state when all
     * pages other than the first one have been decommitted.
     */
    Decommitted
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
} // namespace snmalloc
