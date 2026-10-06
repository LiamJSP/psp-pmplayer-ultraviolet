#include "ppa_scratchpad.h"

#include <pspkernel.h>
#include <stdint.h>
#include <string.h>

#ifndef PPA_ENABLE_SCRATCHPAD_ACCEL
#define PPA_ENABLE_SCRATCHPAD_ACCEL 1
#endif

struct ppa_scratchpad_partition_state {
    volatile SceUID owner;
    volatile unsigned int generation;
};

static volatile enum ppa_scratchpad_phase g_phase = PPA_SCRATCHPAD_PHASE_MENU;
static struct ppa_scratchpad_partition_state
    g_partition[PPA_SCRATCHPAD_PARTITION_COUNT];
static int g_initialized;

static void *partition_base(enum ppa_scratchpad_partition partition)
{
    uintptr_t base = (uintptr_t)PPA_SCRATCHPAD_BASE_ADDRESS;
    switch (partition) {
    case PPA_SCRATCHPAD_PARTITION_VIDEO:
        return (void *)(base + PPA_SCRATCHPAD_VIDEO_OFFSET);
    case PPA_SCRATCHPAD_PARTITION_AUDIO:
        return (void *)(base + PPA_SCRATCHPAD_AUDIO_OFFSET);
    case PPA_SCRATCHPAD_PARTITION_CONTROL:
        return (void *)(base + PPA_SCRATCHPAD_CONTROL_OFFSET);
    default:
        return 0;
    }
}

static unsigned int partition_bytes(enum ppa_scratchpad_partition partition)
{
    switch (partition) {
    case PPA_SCRATCHPAD_PARTITION_VIDEO:
        return PPA_SCRATCHPAD_VIDEO_BYTES;
    case PPA_SCRATCHPAD_PARTITION_AUDIO:
        return PPA_SCRATCHPAD_AUDIO_BYTES;
    case PPA_SCRATCHPAD_PARTITION_CONTROL:
        return PPA_SCRATCHPAD_CONTROL_BYTES;
    default:
        return 0;
    }
}

void ppa_scratchpad_init(void)
{
    int i;
    if (g_initialized)
        return;
    for (i = 0; i < PPA_SCRATCHPAD_PARTITION_COUNT; ++i) {
        g_partition[i].owner = -1;
        g_partition[i].generation = 1U;
    }
    g_phase = PPA_SCRATCHPAD_PHASE_MENU;
    g_initialized = 1;
}

void ppa_scratchpad_set_phase(enum ppa_scratchpad_phase phase)
{
    int i;
    ppa_scratchpad_init();
    if (g_phase == phase)
        return;

    /* Playback/menu transitions are made only after worker joins.  Invalidating
     * all generations makes an accidental stale release harmless. */
    g_phase = phase;
    for (i = 0; i < PPA_SCRATCHPAD_PARTITION_COUNT; ++i) {
        g_partition[i].owner = -1;
        ++g_partition[i].generation;
        if (g_partition[i].generation == 0U)
            g_partition[i].generation = 1U;
    }
}

enum ppa_scratchpad_phase ppa_scratchpad_get_phase(void)
{
    return g_phase;
}

int ppa_scratchpad_acquire(enum ppa_scratchpad_partition partition,
                           struct ppa_scratchpad_lease *out)
{
#if PPA_ENABLE_SCRATCHPAD_ACCEL
    SceUID owner;
    struct ppa_scratchpad_partition_state *state;

    if (out == 0 || partition < 0 ||
        partition >= PPA_SCRATCHPAD_PARTITION_COUNT)
        return 0;
    ppa_scratchpad_init();
    memset(out, 0, sizeof(*out));

    if (g_phase != PPA_SCRATCHPAD_PHASE_PLAYBACK)
        return 0;

    owner = sceKernelGetThreadId();
    state = &g_partition[(int)partition];
    if (state->owner >= 0 && state->owner != owner) {
        return 0;
    }

    state->owner = owner;
    out->base = partition_base(partition);
    out->bytes = partition_bytes(partition);
    out->generation = state->generation;
    out->owner_thread = owner;
    out->partition = partition;
    return out->base != 0 && out->bytes != 0U;
#else
    (void)partition;
    (void)out;
    return 0;
#endif
}

void ppa_scratchpad_release(struct ppa_scratchpad_lease *lease)
{
#if PPA_ENABLE_SCRATCHPAD_ACCEL
    struct ppa_scratchpad_partition_state *state;
    if (lease == 0 || lease->base == 0 || lease->partition < 0 ||
        lease->partition >= PPA_SCRATCHPAD_PARTITION_COUNT)
        return;

    state = &g_partition[(int)lease->partition];
    if (state->generation == lease->generation &&
        state->owner == lease->owner_thread)
        state->owner = -1;
    memset(lease, 0, sizeof(*lease));
#else
    (void)lease;
#endif
}

void ppa_scratchpad_clear_partition(enum ppa_scratchpad_partition partition)
{
#if PPA_ENABLE_SCRATCHPAD_ACCEL
    void *base = partition_base(partition);
    unsigned int bytes = partition_bytes(partition);
    if (base != 0 && bytes != 0U)
        memset(base, 0, bytes);
#else
    (void)partition;
#endif
}
