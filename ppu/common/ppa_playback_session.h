#ifndef PPA_PLAYBACK_SESSION_H
#define PPA_PLAYBACK_SESSION_H

#include <stdint.h>
#include <pspctrl.h>
#include <pspkernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One active movie at a time. Configure on the browser thread before open;
 * close only after every container worker has joined. No callback does I/O. */
struct ppa_session_options {
    int audio_only;
    int health_panel;
    int sleep_minutes;             /* 0 = off, 15-minute steps, maximum 720 */
    int browser_power_lock_owned;  /* exactly one lock borrowed, never invented */
};

struct ppa_session_snapshot {
    int audio_only, paused, audio_valid;
    int64_t audio_ms, video_ms;
    unsigned int decode_us, budget_us, skipped, presented;
    unsigned int original_refs, effective_refs, weights_disabled;
    int compatibility, display_off, suspend_fallback;
};

enum {
    PPA_SESSION_PAUSE = 1,
    PPA_SESSION_CONSUME_INPUT = 2,
    PPA_SESSION_WORKER_AUDIO = 1,
    PPA_SESSION_WORKER_VIDEO = 2,
    PPA_SESSION_WORKER_DECODE = 4
};

void ppa_session_configure(const struct ppa_session_options *options);
int ppa_session_open(void);
void ppa_session_start(void);
int ppa_session_start_workers(SceUID audio, SceUID video, SceUID decode,
                               SceSize arg_size, void *args, volatile int *stop);
void ppa_session_close(void);
int ppa_session_audio_only(void);
int ppa_session_health_enabled(void);
void ppa_session_power_callback(unsigned int power_flags);
void ppa_session_keep_awake(void);
int ppa_session_service(const SceCtrlData *input, int paused);
int ppa_session_pause_point(volatile int *stop, volatile int *paused,
                            unsigned int worker_bit);

/* Authoritative submitted-audio timeline. Hardware queue depth anchors each
 * accepted block; elapsed interpolation never exceeds its queued tail. This
 * is an estimate, not a sample-exact hardware DAC position. Legacy movie
 * resume counters remain container adapters in this first migration phase. */
void ppa_session_set_paused(int paused);
void ppa_session_discontinuity(void);
int ppa_session_seek_begin(unsigned int epoch, int target_ms);
int ppa_session_seek_arrived(unsigned int epoch, int video_ms);
/* Admit an epoch, queue PCM, then publish its remaining-sample clock.
 * Returns -1 on hardware/error, 0 for rejected preroll/epoch, 1 on output. */
int ppa_session_audio_output_epoch(unsigned int epoch, int64_t pts_ms,
                                   unsigned int sample_count,
                                   unsigned int sample_rate,
                                   int volume, void *data);
int ppa_session_audio_time_ms(int fallback_ms);
uint64_t ppa_session_video_target_us(int64_t pts_ms);
void ppa_session_note_present(int64_t pts_ms);
void ppa_session_note_decode(unsigned int elapsed_us, int duration_ms);
void ppa_session_note_skip(void);
void ppa_session_note_compat(unsigned int original_refs,
                            unsigned int effective_refs, int active,
                            unsigned int weights_disabled);
int ppa_session_snapshot(struct ppa_session_snapshot *out);

/* Container adapters provide decoding only; the bounded audio-only producer,
 * pause, cancellation and EOF drain live here. Normal A/V adapters are retained
 * until probe data supports migrating their H264X/seek epoch machinery. */
struct ppa_audio_only_adapter {
    void *owner;
    volatile int *stop, *paused, *eof;
    char * volatile *result;
    SceUID can_put, can_get;
    /* Report native codec + PCM wall time separately from reader/demux waits.
     * This is a conservative scalable-cost estimate, not CPU utilization. */
    char *(*decode)(void *owner, unsigned int *codec_us);
    int (*at_eof)(void *owner, const char *error);
    void (*progress)(void *owner);
    int duration_ms;
    int drain_only; /* A/V EOF tail: same producer, no audio-only governor mode */
};
int ppa_session_run_audio_only(struct ppa_audio_only_adapter *adapter);
void ppa_session_audio_only_consumed(void);
unsigned int ppa_session_audio_only_queued_blocks(void);
/* Both container producers publish before waking output; rejected signals
 * roll back the count. Consumers release once, including stale seek epochs. */
int ppa_session_audio_publish(SceUID can_get);

#ifdef __cplusplus
}
#endif
#endif
