#include "ppa_memory.h"
#include "ppa_hardware_profile.h"

#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspthreadman.h>
#include <stdint.h>
#include <string.h>

#define PPA_MEMORY_PARTITION_ID       2
#define PPA_MEMORY_ALIGNMENT          64U
#define PPA_MEMORY_HEADER_BYTES       64U
#define PPA_MEMORY_MAGIC_FREE         0x504D4652U /* PMFR */
#define PPA_MEMORY_MAGIC_USED         0x504D5553U /* PMUS */
#define PPA_MEMORY_MIN_POOL_BYTES     (4U * 1024U * 1024U)
#define PPA_MEMORY_RESERVE_BYTES      (4U * 1024U * 1024U)
#define PPA_MEMORY_MAX_POOL_BYTES     (24U * 1024U * 1024U)
#define PPA_MEMORY_RETRY_STEP_BYTES   (1U * 1024U * 1024U)

struct ppa_memory_block {
    uint32_t magic;
    uint32_t size;
    uint32_t reserved;
    struct ppa_memory_block *prev;
    struct ppa_memory_block *next;
    unsigned char padding[PPA_MEMORY_HEADER_BYTES -
                          (3U * sizeof(uint32_t)) -
                          (2U * sizeof(void *))];
};

struct ppa_memory_state {
    SceUID partition_block;
    SceUID fixed_region_block;
    SceUID mutex;
    unsigned int fixed_region_attempted;
    unsigned char *base;
    unsigned int bytes;
    struct ppa_memory_block *head;
    unsigned int used;
    unsigned int peak;
    unsigned int allocations;
    unsigned int frees;
    unsigned int fallbacks;
    unsigned int failures;
};

static struct ppa_memory_state g_memory = {
    .partition_block = -1,
    .fixed_region_block = -1,
    .mutex = -1
};

static unsigned int align_up_64(unsigned int value)
{
    return (value + PPA_MEMORY_ALIGNMENT - 1U) &
           ~(PPA_MEMORY_ALIGNMENT - 1U);
}

static unsigned int align_down_megabyte(unsigned int value)
{
    return value & ~((1U * 1024U * 1024U) - 1U);
}

static void memory_lock(void)
{
    if (g_memory.mutex >= 0)
        sceKernelWaitSema(g_memory.mutex, 1, 0);
}

static void memory_unlock(void)
{
    if (g_memory.mutex >= 0)
        sceKernelSignalSema(g_memory.mutex, 1);
}

static struct ppa_memory_block *find_used_block_locked(const void *p)
{
    const unsigned char *q = (const unsigned char *)p;
    struct ppa_memory_block *block;

    if (p == 0 || g_memory.base == 0 ||
        ((uintptr_t)p & (PPA_MEMORY_ALIGNMENT - 1U)) != 0U ||
        q < g_memory.base + PPA_MEMORY_HEADER_BYTES ||
        q >= g_memory.base + g_memory.bytes)
        return 0;

    /* Every arena payload begins exactly one cache-line header after its
     * descriptor. Deriving the header is O(1), unlike walking every block on
     * every free. Link consistency checks keep malformed/interior pointers
     * from being accepted even if payload bytes happen to resemble the magic. */
    block = (struct ppa_memory_block *)(q - PPA_MEMORY_HEADER_BYTES);
    if (block->magic != PPA_MEMORY_MAGIC_USED ||
        block->size == 0U ||
        (block->size & (PPA_MEMORY_ALIGNMENT - 1U)) != 0U ||
        q + block->size > g_memory.base + g_memory.bytes ||
        (block->prev != 0 && block->prev->next != block) ||
        (block->next != 0 && block->next->prev != block))
        return 0;
    return block;
}

static void block_initialize(struct ppa_memory_block *block,
                             unsigned int size,
                             struct ppa_memory_block *prev,
                             struct ppa_memory_block *next)
{
    memset(block, 0, PPA_MEMORY_HEADER_BYTES);
    block->magic = PPA_MEMORY_MAGIC_FREE;
    block->size = size;
    block->prev = prev;
    block->next = next;
}

static int reserve_fixed_extended_region(void)
{
    SceUID block;
    void *head;

    if (g_memory.fixed_region_attempted)
        return g_memory.fixed_region_block >= 0;
    g_memory.fixed_region_attempted = 1U;

    block = sceKernelAllocPartitionMemory(
        PPA_MEMORY_PARTITION_ID,
        "PPA fixed AVC surfaces",
        PSP_SMEM_Addr,
        PPA_MEMORY_EXTENDED_RESERVED_BYTES,
        (void *)(uintptr_t)PPA_MEMORY_EXTENDED_SURFACE_BASE);
    if (block < 0)
        return 0;

    head = sceKernelGetBlockHeadAddr(block);
    if ((uintptr_t)head != (uintptr_t)PPA_MEMORY_EXTENDED_SURFACE_BASE) {
        sceKernelFreePartitionMemory(block);
        return 0;
    }

    g_memory.fixed_region_block = block;
    return 1;
}

int ppa_memory_prepare_fixed_surfaces(void)
{
    ppa_hardware_profile_refresh_memory();
    if (!ppa_hardware_has_64mb_ram())
        return 0;
    if (reserve_fixed_extended_region())
        return 1;
    return 0;
}

int ppa_memory_pool_init(void)
{
    unsigned int max_free;
    unsigned int target;
    SceUID block = -1;
    unsigned char *raw;
    uintptr_t aligned;
    unsigned int skipped;

    if (g_memory.base != 0)
        return 1;

    ppa_hardware_profile_refresh_memory();
    if (!ppa_hardware_has_64mb_ram())
        return 0;

    /*
     * Tell SysMem about the fixed-address Sony AVC/TV-out region before
     * requesting a high arena.  Legacy PPA wrote these addresses without a
     * partition reservation, which means another high allocation could
     * legally overlap them from the kernel allocator's point of view.
     *
     * Some older CFWs may reject PSP_SMEM_Addr for the extended bank.  That is
     * not fatal: the high-allocation retry below still rejects any arena whose
     * base is below the first byte after the legacy region.
     */
    (void)ppa_memory_prepare_fixed_surfaces();

    max_free = (unsigned int)sceKernelMaxFreeMemSize();
    if (max_free <= PPA_MEMORY_RESERVE_BYTES + PPA_MEMORY_MIN_POOL_BYTES)
        return 0;

    target = max_free - PPA_MEMORY_RESERVE_BYTES;
    if (target > PPA_MEMORY_MAX_POOL_BYTES)
        target = PPA_MEMORY_MAX_POOL_BYTES;
    target = align_down_megabyte(target);

    raw = 0;
    while (target >= PPA_MEMORY_MIN_POOL_BYTES) {
        block = sceKernelAllocPartitionMemory(PPA_MEMORY_PARTITION_ID,
                                               "PPA extended arena",
                                               PSP_SMEM_High,
                                               target,
                                               0);
        if (block >= 0) {
            raw = (unsigned char *)sceKernelGetBlockHeadAddr(block);
            if (raw != 0 &&
                (uintptr_t)raw >= PPA_MEMORY_EXTENDED_ARENA_SAFE_BASE)
                break;

            /* The legacy decoder owns 0x0A000000..0x0ADFFFFF for its RGB
             * workspace and eight 768-pitch frame surfaces.  A smaller high
             * allocation begins at a higher address, so retry before giving
             * up on an unusual map. */
            sceKernelFreePartitionMemory(block);
            block = -1;
            raw = 0;
        }
        target -= PPA_MEMORY_RETRY_STEP_BYTES;
    }

    if (block < 0 || raw == 0) {
        g_memory.failures++;
        return 0;
    }

    aligned = ((uintptr_t)raw + PPA_MEMORY_ALIGNMENT - 1U) &
              ~((uintptr_t)PPA_MEMORY_ALIGNMENT - 1U);
    skipped = (unsigned int)(aligned - (uintptr_t)raw);
    if (target <= skipped + PPA_MEMORY_HEADER_BYTES + PPA_MEMORY_ALIGNMENT) {
        sceKernelFreePartitionMemory(block);
        g_memory.failures++;
        return 0;
    }

    g_memory.mutex = sceKernelCreateSema("PPA extended arena lock", 0, 1, 1, 0);
    if (g_memory.mutex < 0) {
        sceKernelFreePartitionMemory(block);
        g_memory.failures++;
        return 0;
    }

    g_memory.partition_block = block;
    g_memory.base = (unsigned char *)aligned;
    g_memory.bytes = (target - skipped) & ~(PPA_MEMORY_ALIGNMENT - 1U);
    g_memory.used = 0;
    g_memory.peak = 0;
    g_memory.head = (struct ppa_memory_block *)g_memory.base;
    block_initialize(g_memory.head,
                     g_memory.bytes - PPA_MEMORY_HEADER_BYTES,
                     0,
                     0);

    return 1;
}

int ppa_memory_pool_owns(const void *p)
{
    const unsigned char *q = (const unsigned char *)p;
    int owns;

    if (p == 0 || g_memory.base == 0 ||
        q < g_memory.base + PPA_MEMORY_HEADER_BYTES ||
        q >= g_memory.base + g_memory.bytes)
        return 0;
    memory_lock();
    owns = find_used_block_locked(p) != 0;
    memory_unlock();
    return owns;
}

void *ppa_memory_large_alloc(unsigned int size)
{
    struct ppa_memory_block *block;
    struct ppa_memory_block *best = 0;
    unsigned int rounded;

    if (size == 0 || g_memory.base == 0)
        return 0;

    rounded = align_up_64(size);
    memory_lock();

    for (block = g_memory.head; block != 0; block = block->next) {
        if (block->magic == PPA_MEMORY_MAGIC_FREE && block->size >= rounded) {
            if (best == 0 || block->size < best->size)
                best = block;
            if (block->size == rounded)
                break;
        }
    }

    if (best == 0) {
        g_memory.failures++;
        memory_unlock();
        return 0;
    }

    if (best->size >= rounded + PPA_MEMORY_HEADER_BYTES + PPA_MEMORY_ALIGNMENT) {
        unsigned char *split_address =
            (unsigned char *)best + PPA_MEMORY_HEADER_BYTES + rounded;
        struct ppa_memory_block *split =
            (struct ppa_memory_block *)split_address;
        unsigned int split_size = best->size - rounded - PPA_MEMORY_HEADER_BYTES;

        block_initialize(split, split_size, best, best->next);
        if (best->next != 0)
            best->next->prev = split;
        best->next = split;
        best->size = rounded;
    }

    best->magic = PPA_MEMORY_MAGIC_USED;
    g_memory.used += best->size;
    if (g_memory.used > g_memory.peak)
        g_memory.peak = g_memory.used;
    g_memory.allocations++;

    memory_unlock();
    return (unsigned char *)best + PPA_MEMORY_HEADER_BYTES;
}

static void coalesce_with_next(struct ppa_memory_block *block)
{
    struct ppa_memory_block *next = block->next;
    if (next == 0 || next->magic != PPA_MEMORY_MAGIC_FREE)
        return;

    block->size += PPA_MEMORY_HEADER_BYTES + next->size;
    block->next = next->next;
    if (block->next != 0)
        block->next->prev = block;
}

void ppa_memory_large_free(void *p)
{
    struct ppa_memory_block *block;

    if (p == 0)
        return;

    memory_lock();
    block = find_used_block_locked(p);
    if (block == 0) {
        g_memory.failures++;
        memory_unlock();
        return;
    }

    block->magic = PPA_MEMORY_MAGIC_FREE;
    if (g_memory.used >= block->size)
        g_memory.used -= block->size;
    else
        g_memory.used = 0;
    g_memory.frees++;

    coalesce_with_next(block);
    if (block->prev != 0 && block->prev->magic == PPA_MEMORY_MAGIC_FREE) {
        block = block->prev;
        coalesce_with_next(block);
    }
    memory_unlock();
}

void ppa_memory_snapshot_get(struct ppa_memory_snapshot *out)
{
    struct ppa_memory_block *block;
    unsigned int free_bytes = 0;
    unsigned int largest = 0;

    if (out == 0)
        return;

    memset(out, 0, sizeof(*out));
    memory_lock();
    for (block = g_memory.head; block != 0; block = block->next) {
        if (block->magic == PPA_MEMORY_MAGIC_FREE) {
            free_bytes += block->size;
            if (block->size > largest)
                largest = block->size;
        }
    }

    out->pool_enabled = g_memory.base != 0;
    out->fixed_region_reserved = g_memory.fixed_region_block >= 0;
    out->fixed_region_base = PPA_MEMORY_EXTENDED_SURFACE_BASE;
    out->fixed_region_bytes = out->fixed_region_reserved ?
                              PPA_MEMORY_EXTENDED_RESERVED_BYTES : 0U;
    out->base_address = (unsigned int)(uintptr_t)g_memory.base;
    out->pool_bytes = g_memory.bytes;
    out->used_bytes = g_memory.used;
    out->peak_used_bytes = g_memory.peak;
    out->free_bytes = free_bytes;
    out->largest_free_bytes = largest;
    out->fragmentation_ppm =
        free_bytes == 0 ? 0U :
        (unsigned int)(((uint64_t)(free_bytes - largest) * 1000000ULL) /
                       (uint64_t)free_bytes);
    out->allocations = g_memory.allocations;
    out->frees = g_memory.frees;
    out->fallbacks = g_memory.fallbacks;
    out->failures = g_memory.failures;
    memory_unlock();
}

void ppa_memory_pool_shutdown(void)
{
    SceUID partition_block;
    SceUID fixed_region_block;
    SceUID mutex;

    /* A timed-out reader may still touch an arena-backed packet. Keep the
     * partition resident until cold restart, even after packet pool close. */
    extern int ppa_packet_pool_me_has_quarantine(void);
    if (ppa_packet_pool_me_has_quarantine())
        return;

    if (g_memory.base == 0 && g_memory.fixed_region_block < 0)
        return;

    partition_block = g_memory.partition_block;
    fixed_region_block = g_memory.fixed_region_block;
    mutex = g_memory.mutex;

    g_memory.partition_block = -1;
    g_memory.fixed_region_block = -1;
    g_memory.mutex = -1;
    g_memory.fixed_region_attempted = 0U;
    g_memory.base = 0;
    g_memory.bytes = 0;
    g_memory.head = 0;
    g_memory.used = 0;

    if (mutex >= 0)
        sceKernelDeleteSema(mutex);
    if (partition_block >= 0)
        sceKernelFreePartitionMemory(partition_block);
    if (fixed_region_block >= 0)
        sceKernelFreePartitionMemory(fixed_region_block);
}

/* Called by mem64.c when the arena was unavailable or exhausted. */
void ppa_memory_note_fallback(void)
{
    __sync_fetch_and_add(&g_memory.fallbacks, 1U);
}

void *ppa_memory_extended_rgb_surface(void)
{
    return (void *)(uintptr_t)PPA_MEMORY_EXTENDED_SURFACE_BASE;
}

void *ppa_memory_extended_frame_surface(unsigned int index)
{
    if (index >= PPA_MEMORY_EXTENDED_FRAME_COUNT)
        return 0;

    return (void *)(uintptr_t)(PPA_MEMORY_EXTENDED_SURFACE_BASE +
                               (index + 1U) *
                               PPA_MEMORY_EXTENDED_FRAME_BYTES);
}

int ppa_memory_fixed_region_reserved(void)
{
    return g_memory.fixed_region_block >= 0;
}
