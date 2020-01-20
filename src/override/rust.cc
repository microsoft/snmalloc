#include "../mem/slowalloc.h"
#include "../snmalloc.h"
#include <cstring>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

using namespace snmalloc;

inline size_t aligned_size(size_t alignment, size_t size) {
    // Client responsible for checking alignment is not zero
    assert(alignment != 0);
    // Client responsible for checking alignment is not above SUPERSLAB_SIZE
    assert(alignment <= SUPERSLAB_SIZE);
    // Client responsible for checking alignment is a power of two
    assert(bits::next_pow2(alignment) == alignment);

    size = bits::max(size, alignment);
    snmalloc::sizeclass_t sc = size_to_sizeclass(size);
    if (sc >= NUM_SIZECLASSES) {
        // large allocs are 16M aligned, which is maximum we guarantee
        return size;
    }
    for (; sc < NUM_SIZECLASSES; sc++) {
        size = sizeclass_to_size(sc);
        if ((size & (~size + 1)) >= alignment) {
            return size;
        }
    }
    // Give max alignment.
    return SUPERSLAB_SIZE;
}

extern "C" SNMALLOC_EXPORT void *rust_alloc(size_t alignment, size_t size) {
    return ThreadAlloc::get_noncachable()->alloc(aligned_size(alignment, size));
}

extern "C" SNMALLOC_EXPORT void rust_dealloc(void *ptr, size_t alignment, size_t size) {
    ThreadAlloc::get_noncachable()->dealloc(ptr, aligned_size(alignment, size));
}

extern "C" SNMALLOC_EXPORT void *rust_realloc(void *ptr, size_t alignment, size_t old_size, size_t new_size) {
    size_t aligned_old_size = aligned_size(alignment, old_size),
            aligned_new_size = aligned_size(alignment, new_size);
    if (aligned_old_size == aligned_new_size) return ptr;
    void *p = ThreadAlloc::get_noncachable()->alloc(aligned_new_size);
    if (p) {
        std::memcpy(p, ptr, old_size < new_size ? old_size : new_size);
        ThreadAlloc::get_noncachable()->dealloc(ptr, aligned_old_size);
    }
    return p;
}
