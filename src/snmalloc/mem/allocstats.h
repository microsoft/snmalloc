#include "../ds_core/ds_core.h"
#include "sizeclasstable.h"

#include <array>

namespace snmalloc
{
  struct AllocStat
  {
    MonotoneLocalStat objects_allocated{};
    MonotoneLocalStat objects_deallocated{};
    MonotoneLocalStat slabs_allocated{};
    MonotoneLocalStat slabs_deallocated{};
  };

  class AllocStats
  {
    std::array<AllocStat, SIZECLASS_REP_SIZE> sizeclass{};

  public:
    AllocStat& operator[](sizeclass_t index)
    {
      auto i = index.raw();
      return sizeclass[i];
    }

    AllocStat& operator[](smallsizeclass_t index)
    {
      return sizeclass[sizeclass_t::from_small_class(index).raw()];
    }

    void operator+=(const AllocStats& other)
    {
      for (size_t i = 0; i < SIZECLASS_REP_SIZE; i++)
      {
        sizeclass[i].objects_allocated += other.sizeclass[i].objects_allocated;
        sizeclass[i].objects_deallocated +=
          other.sizeclass[i].objects_deallocated;
        sizeclass[i].slabs_allocated += other.sizeclass[i].slabs_allocated;
        sizeclass[i].slabs_deallocated += other.sizeclass[i].slabs_deallocated;
      }
    }
  };
} // namespace snmalloc