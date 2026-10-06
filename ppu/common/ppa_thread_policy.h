#ifndef PPA_THREAD_POLICY_H
#define PPA_THREAD_POLICY_H

#include <pspkerneltypes.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ppa_thread_role {
    PPA_THREAD_CALLBACK = 0,
    PPA_THREAD_AUDIO_OUTPUT,
    PPA_THREAD_AUDIO_RESAMPLE,
    PPA_THREAD_VIDEO_PRESENT,
    PPA_THREAD_DEMUX_DECODE,
    PPA_THREAD_PLAYBACK_IO,
    PPA_THREAD_INPUT,
    PPA_THREAD_BROWSER_DIRECTORY,
    PPA_THREAD_BROWSER_METADATA,
    PPA_THREAD_BROWSER_CACHE,
    PPA_THREAD_BACKGROUND
};

int ppa_thread_priority(enum ppa_thread_role role);
SceUID ppa_thread_create(enum ppa_thread_role role,
                         const char *name,
                         SceKernelThreadEntry entry,
                         int stack_size,
                         unsigned int extra_attributes);

#ifdef __cplusplus
}
#endif

#endif
