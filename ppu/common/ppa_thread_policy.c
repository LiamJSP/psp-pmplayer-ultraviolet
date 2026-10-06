#include "ppa_thread_policy.h"

#include <pspkernel.h>

int ppa_thread_priority(enum ppa_thread_role role)
{
    switch (role) {
    case PPA_THREAD_CALLBACK: return 0x10;
    case PPA_THREAD_INPUT: return 0x12;
    case PPA_THREAD_AUDIO_OUTPUT: return 0x18;
    case PPA_THREAD_VIDEO_PRESENT: return 0x20;
    /* Required producer-side resampling runs ahead of demux/decode but yields
     * to both the hardware audio deadline and the current video presentation
     * deadline. This avoids trading a short audio conversion for visible
     * presentation jitter. */
    case PPA_THREAD_AUDIO_RESAMPLE: return 0x24;
    case PPA_THREAD_DEMUX_DECODE: return 0x28;
    /* One event-driven worker serializes playback storage below decode. */
    case PPA_THREAD_PLAYBACK_IO: return 0x32;
    case PPA_THREAD_BROWSER_DIRECTORY: return 0x40;
    case PPA_THREAD_BROWSER_METADATA: return 0x44;
    case PPA_THREAD_BROWSER_CACHE: return 0x48;
    case PPA_THREAD_BACKGROUND: return 0x50;
    default: return 0x28;
    }
}

SceUID ppa_thread_create(enum ppa_thread_role role,
                         const char *name,
                         SceKernelThreadEntry entry,
                         int stack_size,
                         unsigned int extra_attributes)
{
    unsigned int attributes = PSP_THREAD_ATTR_USER | extra_attributes;
    SceUID id;
    id = sceKernelCreateThread(name,
                               entry,
                                      ppa_thread_priority(role),
                                      stack_size,
                                      attributes,
                                      0);
    
    return id;
}
