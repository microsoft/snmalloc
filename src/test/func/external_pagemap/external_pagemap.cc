#if defined(_WIN32) || !defined(TODO_REINSTATE_POSSIBLY)
// This test does not make sense with malloc pass-through, skip it.
// The malloc definitions are also currently incompatible with Windows headers
// so skip this test on Windows as well.
int main()
{
  return 0;
}
#else
#  define SNMALLOC_EXPOSE_PAGEMAP 1
#  include <snmalloc/override/malloc.cc>

using ExternalChunkmap =
  ExternalGlobalPagemapTemplate<ChunkmapPagemap, snmalloc_chunkmap_global_get>;

int main()
{
  auto& p = ExternalChunkmap::pagemap();
  auto& global = GlobalChunkmap::pagemap();
  SNMALLOC_CHECK(&p == &global);
  // Get a valid heap address
  uintptr_t addr = unsafe_to_uintptr<void>(malloc(42));
  // Make this very strongly aligned
  addr &= ~0xfffffULL;
  void* page = p.page_for_address(addr);
  SNMALLOC_CHECK(page == p.page_for_address(addr + 128));
  size_t idx = p.index_for_address(addr);
  size_t idx2 = p.index_for_address(addr + SUPERSLAB_SIZE);
  // If the pagemap ends up storing things that are not uint8_t, this test
  // will need modifying.
  SNMALLOC_CHECK(idx2 = ((idx + 1) % OS_PAGE_SIZE));
}
#endif
