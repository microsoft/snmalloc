#pragma once

#include "../ds/mpmcstack.h"
#include "allocconfig.h"

#include <type_traits>

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

  class Superslab;
  class Mediumslab;
  class Largeslab;
  class Deallocslab;

  class Baseslab
  {
  protected:
    friend class Superslab;
    friend class Mediumslab;
    friend class Largeslab;
    friend class Deallocslab;
    SlabKind kind;

  public:
    SlabKind get_kind()
    {
      return kind;
    }
  };

  static_assert(
    std::is_standard_layout_v<Baseslab>, "Baseslab not standard layout");
} // namespace snmalloc
