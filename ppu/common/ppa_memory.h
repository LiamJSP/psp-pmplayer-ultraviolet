#ifndef PPA_MEMORY_H
#define PPA_MEMORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Slim/Brite fixed-address compatibility region.
 *
 * The legacy Sony AVC/TV-out path uses 0x0A000000 as its RGB workspace and
 * eight 0x180000-byte display surfaces immediately above it.  The last frame
 * ends at 0x0AD80000, so the partition allocator must never hand that range to
 * the general large-object arena.
 */
#define PPA_MEMORY_EXTENDED_SURFACE_BASE       0x0A000000U
#define PPA_MEMORY_EXTENDED_FRAME_BYTES        0x00180000U
#define PPA_MEMORY_EXTENDED_FRAME_COUNT        8U
#define PPA_MEMORY_EXTENDED_RESERVED_BYTES     0x00E00000U
#define PPA_MEMORY_EXTENDED_ARENA_SAFE_BASE    0x0AE00000U

struct ppa_memory_snapshot {
    unsigned int pool_enabled;
    unsigned int fixed_region_reserved;
    unsigned int fixed_region_base;
    unsigned int fixed_region_bytes;
    unsigned int base_address;
    unsigned int pool_bytes;
    unsigned int used_bytes;
    unsigned int peak_used_bytes;
    unsigned int free_bytes;
    unsigned int largest_free_bytes;
    unsigned int fragmentation_ppm;
    unsigned int allocations;
    unsigned int frees;
    unsigned int fallbacks;
    unsigned int failures;
};

/* Reserve the legacy Slim/Brite fixed surfaces before graphics starts. */
int ppa_memory_prepare_fixed_surfaces(void);

/* Call after required PRX/codec modules are loaded. */
int ppa_memory_pool_init(void);
void ppa_memory_pool_shutdown(void);
void *ppa_memory_large_alloc(unsigned int size);
void ppa_memory_large_free(void *p);
int ppa_memory_pool_owns(const void *p);
void ppa_memory_snapshot_get(struct ppa_memory_snapshot *out);

void ppa_memory_note_fallback(void);

/* Fixed-address surfaces retained for compatibility with Sony AVC/TV-out. */
void *ppa_memory_extended_rgb_surface(void);
void *ppa_memory_extended_frame_surface(unsigned int index);
int ppa_memory_fixed_region_reserved(void);

#ifdef __cplusplus
}
#endif

#endif
