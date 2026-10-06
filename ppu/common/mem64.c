/*
 * 64-byte aligned allocation for PSP cache/DMA sharing.
 *
 * Large allocations use the optional high-address partition arena on
 * Slim/Brite/Go/Street hardware when the CFW has exposed extended RAM. Small
 * objects remain on newlib's heap so allocator metadata and tiny lifetimes do
 * not fragment the large streaming arena.
 */

#include "mem64.h"
#include <limits.h>
#include "ppa_memory.h"

#define PPA_MEMORY_LARGE_THRESHOLD (64 * 1024)

void *malloc_64(int size)
{
    int mod_64;
    void *p;

    if (size <= 0 || size > INT_MAX - 63)
        return 0;

    mod_64 = size & 0x3f;
    if (mod_64 != 0)
        size += 64 - mod_64;

    if (size >= PPA_MEMORY_LARGE_THRESHOLD) {
        p = ppa_memory_large_alloc((unsigned int)size);
        if (p != 0)
            return p;
        ppa_memory_note_fallback();
    }

    return memalign(64, (size_t)size);
}

void free_64(void *p)
{
    if (p == 0)
        return;

    if (ppa_memory_pool_owns(p)) {
        ppa_memory_large_free(p);
        return;
    }

    free(p);
}
