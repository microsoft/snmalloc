#include <iostream>

#if !defined(__CHERI_PURE_CAPABILITY__)
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
    void* olarge = alloc->alloc<Zero>(sz);
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
   * Grab another Alloc pointer from the pool and examine it.
   *
   * Alloc-s come from the metadata pools of snmalloc, and so do not flow
   * through the usual allocation machinery.
   */
  message("Grab Alloc from pool for inspection");
  {
    static_assert(
      std::is_same_v<decltype(alloc.alloc), Allocator<StandardConfig>>);

    auto* ca = AllocPool<StandardConfig>::acquire();

    SNMALLOC_CHECK(cap_len_is(ca, sizeof(*ca)));
    SNMALLOC_CHECK(cap_vmem_perm_is(ca, false));

    /*
     * Putting ca back into the pool would require unhooking our local cache,
     * and that requires accessing privates.  Since it's pretty harmless to do
     * so here at the end of our test, just leak it.
     */
  }

  /*
   * Verify that our memcpy implementation successfully copies capabilities
   * even when it is given a region that is not capability-aligned.
   */
  message("Checking memcpy behaviors");
  {
    static constexpr size_t ncaps = 16;

    int* icaps[ncaps];

    for (size_t i = 0; i < ncaps; i++)
    {
      icaps[i] = (int*)&icaps[i];
      SNMALLOC_CHECK(__builtin_cheri_tag_get(icaps[i]));
    }

    int* ocaps[ncaps];

    /*
     * While it may seem trivial, check the both-aligned case, both for one
     * and for many capabilities.
     */
    bzero(ocaps, sizeof(ocaps));
    snmalloc::memcpy<false>(ocaps, icaps, sizeof(void*));
    SNMALLOC_CHECK(__builtin_cheri_tag_get(ocaps[0]));
    SNMALLOC_CHECK(__builtin_cheri_equal_exact(icaps[0], ocaps[0]));

    bzero(ocaps, sizeof(ocaps));
    snmalloc::memcpy<false>(ocaps, icaps, sizeof(icaps));
    for (size_t i = 0; i < ncaps; i++)
    {
      SNMALLOC_CHECK(__builtin_cheri_tag_get(ocaps[i]));
      SNMALLOC_CHECK(__builtin_cheri_equal_exact(icaps[i], ocaps[i]));
    }

    /*
     * When both input and output are equally misaligned, we should preserve
     * caps that aren't sheared by the copy.  The size of this copy is also
     * "unnatural", which should guarantee that any memcpy implementation that
     * tries the overlapping-misaligned-sizeof(long)-at-the-end dance corrupts
     * the penultimate capability by overwriting it with (identical) data.
     *
     * Probe a misaligned copy of bytes followed by a zero or more pointers
     * followed by bytes.
     */
    for (size_t pre = 1; pre < sizeof(int*); pre++)
    {
      for (size_t post = 0; post < sizeof(int*); post++)
      {
        for (size_t ptrs = 0; ptrs < ncaps - 2; ptrs++)
        {
          bzero(ocaps, sizeof(ocaps));

          snmalloc::memcpy<false>(
            pointer_offset(ocaps, pre),
            pointer_offset(icaps, pre),
            (ptrs + 1) * sizeof(int*) - pre + post);

          /* prefix */
          SNMALLOC_CHECK(
            memcmp(
              pointer_offset(icaps, pre),
              pointer_offset(ocaps, pre),
              sizeof(int*) - pre) == 0);
          /* pointer */
          for (size_t p = 0; p < ptrs; p++)
          {
            SNMALLOC_CHECK(__builtin_cheri_tag_get(ocaps[1 + p]));
            SNMALLOC_CHECK(
              __builtin_cheri_equal_exact(icaps[1 + p], ocaps[1 + p]));
          }
          /* suffix */
          SNMALLOC_CHECK(memcmp(&icaps[1 + ptrs], &ocaps[1 + ptrs], post) == 0);
        }
      }
    }

    /*
     * If the alignments are different, then the bytes should get copied but
     * the tags should be cleared.
     */
    for (size_t sa = 0; sa < sizeof(int*); sa++)
    {
      for (size_t da = 0; da < sizeof(int*); da++)
      {
        static constexpr size_t n = 4;

        if (sa == da)
        {
          continue;
        }

        bzero(ocaps, n * sizeof(int*));

        snmalloc::memcpy<false>(
          pointer_offset(ocaps, da),
          pointer_offset(icaps, sa),
          n * sizeof(int*) - da - sa);

        for (size_t i = 0; i < n; i++)
        {
          SNMALLOC_CHECK(__builtin_cheri_tag_get(ocaps[i]) == 0);
        }

        SNMALLOC_CHECK(
          memcmp(
            pointer_offset(icaps, sa),
            pointer_offset(ocaps, da),
            n * sizeof(int*) - da - sa) == 0);
      }
    }
  }

  message("Verify sizeclass representability");
  {
    for (size_t sc = 0; sc < NUM_SMALL_SIZECLASSES; sc++)
    {
      size_t sz = sizeclass_full_to_size(sizeclass_t::from_small_class(sc));
      SNMALLOC_CHECK(sz == Aal::capptr_size_round(sz));
    }

    for (size_t sc = 0; sc < bits::BITS; sc++)
    {
      size_t sz = sizeclass_full_to_size(sizeclass_t::from_large_class(sc));
      SNMALLOC_CHECK(sz == Aal::capptr_size_round(sz));
    }
  }

  message("CHERI checks OK");
  return 0;
}

#endif
