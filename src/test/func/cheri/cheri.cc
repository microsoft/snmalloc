#include <iostream>

#if defined(SNMALLOC_PASS_THROUGH) || !defined(__CHERI_PURE_CAPABILITY__)
// This test does not make sense in pass-through or w/o CHERI
int main()
{
  return 0;
}
#else

// #  define SNMALLOC_TRACING

#  include <cheri/cherireg.h>
#  include <snmalloc/snmalloc.h>
#  include <stddef.h>

#  if defined(__FreeBSD__)
#    include <sys/mman.h>
#  endif

using namespace snmalloc;

bool cap_len_is(void* cap, size_t expected)
{
  return __builtin_cheri_length_get(cap) == expected;
}

bool cap_vmem_perm_is(void* cap, bool expected)
{
#  if defined(CHERI_PERM_SW_VMEM)
  return !!(__builtin_cheri_perms_get(cap) & CHERI_PERM_SW_VMEM) == expected;
#  else
#    warning "Don't know how to check VMEM permission bit"
#  endif
}

int main()
{

#  if defined(__FreeBSD__)
  {
    size_t pagesize[8];
    int err = getpagesizes(pagesize, sizeof(pagesize) / sizeof(pagesize[0]));
    SNMALLOC_CHECK(err > 0);
    SNMALLOC_CHECK(pagesize[0] == OS_PAGE_SIZE);
  }
#  endif

  auto alloc = get_scoped_allocator();

  message("Grab small object");
  {
    static const size_t sz = 128;
    void* o1 = alloc->alloc(sz);
    SNMALLOC_CHECK(cap_len_is(o1, sz));
    SNMALLOC_CHECK(cap_vmem_perm_is(o1, false));
    alloc->dealloc(o1);
  }

  /*
   * This large object is sized to end up in our alloc's local buddy allocators
   * when it's released.
   */
  message("Grab large object");
  ptraddr_t alarge;
  {
    static const size_t sz = 1024 * 1024;
    void* olarge = alloc->alloc(sz);
    alarge = address_cast(olarge);
    SNMALLOC_CHECK(cap_len_is(olarge, sz));
    SNMALLOC_CHECK(cap_vmem_perm_is(olarge, false));

    static_cast<uint8_t*>(olarge)[128] = 'x';
    static_cast<uint8_t*>(olarge)[128 + OS_PAGE_SIZE] = 'y';

#  if defined(__FreeBSD__)
    static constexpr int irm =
      MINCORE_INCORE | MINCORE_REFERENCED | MINCORE_MODIFIED;
    char ic[2];
    int err = mincore(olarge, 2 * OS_PAGE_SIZE, ic);
    SNMALLOC_CHECK(err == 0);
    SNMALLOC_CHECK((ic[0] & irm) == irm);
    SNMALLOC_CHECK((ic[1] & irm) == irm);
    message("Large object in core; good");
#  endif

    alloc->dealloc(olarge);
  }

  message("Grab large object again, verify reuse");
  {
    static const size_t sz = 1024 * 1024;
    errno = 0;
    void* olarge = alloc->alloc<YesZero>(sz);
    int err = errno;

    SNMALLOC_CHECK(alarge == address_cast(olarge));
    SNMALLOC_CHECK(err == 0);

#  if defined(__FreeBSD__)
    /*
     * Verify that the zeroing took place by mmap, which should mean that the
     * first two pages are not in core.  This implies that snmalloc successfully
     * re-derived a Chunk- or Arena-bounded pointer and used that, and its VMAP
     * permission, to tear pages out of the address space.
     */
    static constexpr int irm =
      MINCORE_INCORE | MINCORE_REFERENCED | MINCORE_MODIFIED;
    char ic[2];
    err = mincore(olarge, 2 * OS_PAGE_SIZE, ic);
    SNMALLOC_CHECK(err == 0);
    SNMALLOC_CHECK((ic[0] & irm) == 0);
    SNMALLOC_CHECK((ic[1] & irm) == 0);
    message("Large object not in core; good");
#  endif

    SNMALLOC_CHECK(static_cast<uint8_t*>(olarge)[128] == '\0');
    SNMALLOC_CHECK(static_cast<uint8_t*>(olarge)[128 + OS_PAGE_SIZE] == '\0');
    SNMALLOC_CHECK(cap_len_is(olarge, sz));
    SNMALLOC_CHECK(cap_vmem_perm_is(olarge, false));

    alloc->dealloc(olarge);
  }

  /*
   * Grab another CoreAlloc pointer from the pool and examine it.
   *
   * CoreAlloc-s come from the metadata pools of snmalloc, and so do not flow
   * through the usual allocation machinery.
   */
  message("Grab CoreAlloc from pool for inspection");
  {
    static_assert(
      std::is_same_v<decltype(alloc.alloc), LocalAllocator<StandardConfig>>);

    LocalCache lc{&StandardConfig::unused_remote};
    auto* ca = AllocPool<StandardConfig>::acquire(&lc);

    SNMALLOC_CHECK(cap_len_is(ca, sizeof(*ca)));
    SNMALLOC_CHECK(cap_vmem_perm_is(ca, false));

    /*
     * Putting ca back into the pool would require unhooking our local cache,
     * and that requires accessing privates.  Since it's pretty harmless to do
     * so here at the end of our test, just leak it.
     */
  }

  message("CHERI checks OK");
  return 0;
}

#endif
