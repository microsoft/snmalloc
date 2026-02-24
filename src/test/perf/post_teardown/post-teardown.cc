#include <cstdlib>
#include <test/measuretime.h>
#include <test/setup.h>
#include <test/snmalloc_testlib.h>
#include <vector>

using namespace snmalloc;

void fill(std::vector<void*>& out, size_t count, size_t size)
{
  out.reserve(count);
  for (size_t i = 0; i < count; i++)
  {
    out.push_back(snmalloc::alloc<Uninit>(size));
  }
}

void drain(const char* label, std::vector<void*>& vec, size_t size)
{
  MeasureTime m;
  m << label << " (" << vec.size() << " x " << size << " B)";
  for (void* p : vec)
  {
    snmalloc::dealloc(p, size);
  }
  vec.clear();
}

int main(int, char**)
{
  setup();
  // Issue #809: perf when many objects are freed after the allocator has
  // already been finalised (e.g. static/global teardown). Keep counts equal
  // for baseline and post-teardown to isolate the teardown cost.
  constexpr size_t alloc_count = 1 << 18;
  constexpr size_t obj_size = 64;

  std::vector<void*> ptrs;
  fill(ptrs, alloc_count, obj_size);
  drain("Baseline dealloc before finalise", ptrs, obj_size);

  // Simulate the allocator already being torn down before remaining frees
  // (post-main / static destruction path from #809).
  snmalloc::debug_teardown();

  fill(ptrs, alloc_count, obj_size);
  drain("Immediate dealloc after teardown", ptrs, obj_size);

  return 0;
}
