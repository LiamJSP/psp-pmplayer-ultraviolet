#ifndef PPA_SCRATCHPAD_H
#define PPA_SCRATCHPAD_H

#include <stddef.h>
#include <pspkerneltypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allegrex exposes 16 KiB of on-die scratchpad SRAM at 0x00010000.  Keep the
 * partitions fixed so video and audio never serialize behind a global lock.
 * The control partition is reserved for tiny shared constants/descriptors;
 * it must never contain long-lived application state. */
#define PPA_SCRATCHPAD_BASE_ADDRESS   0x00010000U
#define PPA_SCRATCHPAD_TOTAL_BYTES    0x00004000U
#define PPA_SCRATCHPAD_VIDEO_OFFSET   0x00000000U
#define PPA_SCRATCHPAD_VIDEO_BYTES    0x00003000U
#define PPA_SCRATCHPAD_AUDIO_OFFSET   0x00003000U
#define PPA_SCRATCHPAD_AUDIO_BYTES    0x00000800U
#define PPA_SCRATCHPAD_CONTROL_OFFSET 0x00003800U
#define PPA_SCRATCHPAD_CONTROL_BYTES  0x00000800U

enum ppa_scratchpad_phase {
    PPA_SCRATCHPAD_PHASE_MENU = 0,
    PPA_SCRATCHPAD_PHASE_PLAYBACK,
    PPA_SCRATCHPAD_PHASE_SUSPENDED
};

enum ppa_scratchpad_partition {
    PPA_SCRATCHPAD_PARTITION_VIDEO = 0,
    PPA_SCRATCHPAD_PARTITION_AUDIO,
    PPA_SCRATCHPAD_PARTITION_CONTROL,
    PPA_SCRATCHPAD_PARTITION_COUNT
};

struct ppa_scratchpad_lease {
    void *base;
    unsigned int bytes;
    unsigned int generation;
    SceUID owner_thread;
    enum ppa_scratchpad_partition partition;
};

void ppa_scratchpad_init(void);
void ppa_scratchpad_set_phase(enum ppa_scratchpad_phase phase);
enum ppa_scratchpad_phase ppa_scratchpad_get_phase(void);

/* Non-blocking, partition-local ownership.  A failed acquisition must fall
 * back immediately; callers must never wait on optional acceleration. */
int ppa_scratchpad_acquire(enum ppa_scratchpad_partition partition,
                           struct ppa_scratchpad_lease *out);
void ppa_scratchpad_release(struct ppa_scratchpad_lease *lease);
void ppa_scratchpad_clear_partition(enum ppa_scratchpad_partition partition);

#ifdef __cplusplus
}
#endif

#endif
