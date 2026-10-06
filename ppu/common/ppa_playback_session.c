#include "../mod/audio_stream.h"
#include "ppa_playback_session.h"
#include "ppa_privileged_bridge.h"
#include "ppa_wait.h"
#include "ppa_playback_control.h"
#include "ppa_bus_manager.h"
#include "../mod/cpu_clock.h"
#include <pspaudio.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <string.h>
#include <limits.h>

static struct ppa_session_options g_options;
static struct {
    SceUID lock;
    struct ppa_session_snapshot stats;
    uint64_t audio_wall_us, audio_duration_us;
    uint64_t anchor_wall_us;
    int64_t anchor_ms, last_video_ms;
    int anchor_valid;
    unsigned int audio_epoch, seek_pending;
    int audio_min_ms;
} g_timeline = { .lock = -1 };
static volatile unsigned int g_power_events, g_idle_workers, g_audio_pending;
/* Main-thread-only policy. Workers never read these multiword values. */
static uint64_t g_started_us, g_last_input_us, g_suspend_wait_us;
static int g_active, g_timer_fired, g_suspend_pending, g_suspend_requested;
static int g_saw_suspend, g_lock_borrowed, g_consume_wake, g_display_off;
static int g_blank_pending;
static void *g_saved_frame;
static int g_saved_pitch, g_saved_format;

static uint64_t session_now(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }
static unsigned int atomic_read(volatile unsigned int *p)
{ return __sync_fetch_and_add(p, 0U); }

static int timeline_lock(void)
{
    SceUInt timeout = 2000U;
    return g_timeline.lock >= 0 &&
           sceKernelWaitSema(g_timeline.lock, 1, &timeout) >= 0;
}
static void timeline_unlock(void) { sceKernelSignalSema(g_timeline.lock, 1); }

/* Optional telemetry must never add a semaphore wait to the audio/video
 * schedule. A missed sample leaves the previous panel value in place. The
 * authoritative audio timeline retains its existing synchronization. */
static int timeline_try_lock(void)
{
    return g_timeline.lock >= 0 && sceKernelPollSema(g_timeline.lock, 1) >= 0;
}

static int64_t audio_time_locked(uint64_t now)
{
    uint64_t delta = now >= g_timeline.audio_wall_us ?
                     now - g_timeline.audio_wall_us : 0;
    if (g_timeline.stats.paused) delta = 0;
    if (delta > g_timeline.audio_duration_us) delta = g_timeline.audio_duration_us;
    return g_timeline.stats.audio_ms + (int64_t)(delta / 1000ULL);
}

void ppa_session_configure(const struct ppa_session_options *options)
{
    memset(&g_options, 0, sizeof(g_options));
    if (options) g_options = *options;
    if (g_options.sleep_minutes < 0) g_options.sleep_minutes = 0;
    if (g_options.sleep_minutes > 720) g_options.sleep_minutes = 720;
    g_options.sleep_minutes -= g_options.sleep_minutes % 15;
}

int ppa_session_open(void)
{
    if (g_active) return -1;
    memset(&g_timeline, 0, sizeof(g_timeline));
    g_timeline.lock = sceKernelCreateSema("ppa_timeline", 0, 1, 1, 0);
    if (g_timeline.lock < 0) return -1;
    g_timeline.stats.audio_only = g_options.audio_only;
    g_timeline.stats.video_ms = -1;
    g_timeline.last_video_ms = -1;
    g_idle_workers = g_audio_pending = g_power_events = 0U;
    g_started_us = g_last_input_us = g_suspend_wait_us = 0;
    g_timer_fired = g_suspend_pending = g_suspend_requested = 0;
    g_saw_suspend = g_lock_borrowed = g_consume_wake = g_display_off = 0;
    g_blank_pending = 0;
    g_saved_frame = 0;
    g_active = 1;
    return 0;
}

void ppa_session_start(void)
{
    g_started_us = g_last_input_us = session_now();
    g_idle_workers = 0U;
}
int ppa_session_start_workers(SceUID audio, SceUID video, SceUID decode,
                               SceSize arg_size, void *args, volatile int *stop)
{
    SceUID workers[3] = { audio, video, decode };
    int i, started[3] = {0, 0, 0};
    for (i = 0; i < 3; ++i) {
        if (workers[i] < 0) continue; /* audio-only has no video worker */
        if (sceKernelStartThread(workers[i], arg_size, args) < 0) {
            int j;
            *stop = 1;
            for (j = 0; j < i; ++j) {
                if (started[j] && ppa_wait_thread_end_safe(workers[j],
                        "session", "partial_start", 2000000U, 250000U) < 0)
                    ppa_wait_quarantine("Playback partial start did not stop", -1);
            }
            return -1;
        }
        started[i] = 1;
    }
    return 0;
}

int ppa_session_audio_only(void) { return g_options.audio_only != 0; }
int ppa_session_health_enabled(void) { return g_options.health_panel != 0; }

static void session_restore_power_lock(void)
{
    if (g_lock_borrowed) {
        scePowerLock(0);
        g_lock_borrowed = 0;
    }
}

static void session_fallback_blank(void)
{
    if (!g_blank_pending) return;
    if (!g_options.audio_only) {
        unsigned int wanted = PPA_SESSION_WORKER_AUDIO |
                              PPA_SESSION_WORKER_VIDEO |
                              PPA_SESSION_WORKER_DECODE;
        /* A worker still inside a display/decoder call can retire the current
         * lease after we save it. Native display-off is safe without changing
         * surfaces; defer the null-scanout fallback until ownership is stable. */
        if ((atomic_read(&g_idle_workers) & wanted) != wanted) return;
    }
    if (sceDisplayGetFrameBuf(&g_saved_frame, &g_saved_pitch,
            &g_saved_format, PSP_DISPLAY_SETBUF_IMMEDIATE) < 0) return;
    if (sceDisplaySetFrameBuf(0, 0, PSP_DISPLAY_PIXEL_FORMAT_8888,
                              PSP_DISPLAY_SETBUF_IMMEDIATE) < 0) {
        g_saved_frame = 0;
        return;
    }
    g_blank_pending = 0;
}

static void session_display(int enabled)
{
    if (enabled) {
        if (!g_display_off) return;
        g_blank_pending = 0;
        (void)ppa_privileged_display_set_enabled(1);
        if (g_saved_frame)
            sceDisplaySetFrameBuf(g_saved_frame, g_saved_pitch, g_saved_format,
                                  PSP_DISPLAY_SETBUF_IMMEDIATE);
        g_saved_frame = 0;
        g_display_off = 0;
    } else {
        if (g_display_off) return;
        /* The narrow bridge turns off the backlight as well as scanout. On a
         * firmware/bridge failure, blank scanout without changing GE ownership.
         * That last fallback cannot promise physical backlight power savings. */
        if (ppa_privileged_display_set_enabled(0) < 0) {
            g_blank_pending = 1;
            session_fallback_blank();
        }
        g_display_off = 1;
    }
}

void ppa_session_close(void)
{
    if (!g_active) return;
    session_display(1);
    session_restore_power_lock();
    g_active = 0;
    sceKernelDeleteSema(g_timeline.lock);
    g_timeline.lock = -1;
}

void ppa_session_power_callback(unsigned int flags)
{
    /* Always just an atomic mailbox; no locks, display calls or worker waits. */
    __sync_fetch_and_or(&g_power_events, flags);
}

void ppa_session_keep_awake(void)
{
    if (!g_suspend_requested)
        scePowerTick((g_options.audio_only || g_display_off) ?
                     PSP_POWER_TICK_SUSPEND : PSP_POWER_TICK_ALL);
}

int ppa_session_service(const SceCtrlData *input, int paused)
{
    uint64_t now = session_now();
    unsigned int events = __sync_lock_test_and_set(&g_power_events, 0U);
    int actions = 0;
    ppa_session_set_paused(paused);
    int activity = input && ((input->Buttons & ~PSP_CTRL_HOLD) != 0U ||
                  input->Lx < 104 || input->Lx > 152 ||
                  input->Ly < 104 || input->Ly > 152);

    if (events & (PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_STANDBY)) {
        g_saw_suspend = 1;
        actions |= PPA_SESSION_PAUSE;
    }
    if (events & PSP_POWER_CB_RESUME_COMPLETE) {
        g_suspend_pending = g_suspend_requested = 0;
        session_restore_power_lock();
        session_display(1);
        g_last_input_us = now;
        g_consume_wake = 1;
        ppa_session_discontinuity();
        actions |= PPA_SESSION_PAUSE; /* resume hardware, keep movie paused */
    }
    if (g_started_us && !g_timer_fired && g_options.sleep_minutes > 0 &&
        now - g_started_us >= (uint64_t)g_options.sleep_minutes * 60000000ULL) {
        g_timer_fired = 1;
        g_suspend_pending = 1;
        g_suspend_wait_us = now;
        actions |= PPA_SESSION_PAUSE | PPA_SESSION_CONSUME_INPUT;
    }
    if (g_suspend_pending) {
        unsigned int wanted = PPA_SESSION_WORKER_AUDIO | PPA_SESSION_WORKER_DECODE;
        if (!g_options.audio_only) wanted |= PPA_SESSION_WORKER_VIDEO;
        actions |= PPA_SESSION_PAUSE | PPA_SESSION_CONSUME_INPUT;
        if (!g_suspend_requested && paused &&
            (atomic_read(&g_idle_workers) & wanted) == wanted &&
            sceAudioGetChannelRestLength(0) <= 0) {
            /* Browser normally locks suspend around synchronous playback.
             * Borrow exactly its existing lock for this deliberate transition,
             * restoring it on resume/failure before browser cleanup unlocks. */
            if (g_options.browser_power_lock_owned && scePowerUnlock(0) >= 0)
                g_lock_borrowed = 1;
            g_saw_suspend = 0;
            g_suspend_requested = 1;
            g_suspend_wait_us = now;
            (void)scePowerRequestSuspend(); /* PSPSDK: return value is always 0 */
        }
        if (!g_saw_suspend && now - g_suspend_wait_us >= 2000000ULL) {
            session_restore_power_lock();
            g_suspend_pending = g_suspend_requested = 0;
            session_display(0);
            if (timeline_lock()) {
                g_timeline.stats.suspend_fallback = 1;
                timeline_unlock();
            }
        }
    }

    if (g_blank_pending && !activity) session_fallback_blank();
    if (activity) {
        g_last_input_us = now;
        /* Audio-only keep-awake intentionally leaves the firmware LCD timer
         * running. A real input must restart that timer after a display wake. */
        if (!g_suspend_requested) scePowerTick(PSP_POWER_TICK_DISPLAY);
        if (g_display_off) {
            session_display(1);
            g_consume_wake = 1;
        }
    } else if (g_options.audio_only && g_started_us && !g_suspend_pending &&
               now - g_last_input_us >= 30000000ULL) {
        session_display(0);
    }
    if (g_consume_wake) {
        actions |= PPA_SESSION_CONSUME_INPUT;
        if (!activity) g_consume_wake = 0;
    }
    if (timeline_lock()) {
        g_timeline.stats.display_off = g_display_off;
        timeline_unlock();
    }
    return actions;
}

int ppa_session_pause_point(volatile int *stop, volatile int *paused,
                            unsigned int worker_bit)
{
    if (*paused && !*stop) {
        __sync_fetch_and_or(&g_idle_workers, worker_bit);
        while (*paused && !*stop) sceKernelDelayThread(5000U);
        __sync_fetch_and_and(&g_idle_workers, ~worker_bit);
    }
    return !*stop;
}

void ppa_session_set_paused(int paused)
{
    if (!timeline_lock()) return;
    if ((paused != 0) == g_timeline.stats.paused) { timeline_unlock(); return; }
    if (paused && !g_timeline.stats.paused && g_timeline.stats.audio_valid)
        g_timeline.stats.audio_ms = audio_time_locked(session_now());
    g_timeline.stats.paused = paused != 0;
    g_timeline.audio_wall_us = session_now();
    g_timeline.audio_duration_us = 0;
    g_timeline.anchor_valid = 0;
    timeline_unlock();
}
void ppa_session_discontinuity(void)
{
    if (!timeline_lock()) return;
    g_timeline.anchor_valid = 0;
    g_timeline.stats.audio_valid = 0;
    g_timeline.last_video_ms = -1;
    timeline_unlock();
}
/* A buffer can pass a container's epoch check just before the producer
 * resets that epoch. Validate admission again under the clock's own lock.
 * The timestamp floor survives video arrival, so queued new-epoch preroll
 * cannot become audible or pull the normal-playback clock backward. */
int ppa_session_seek_begin(unsigned int epoch, int target_ms)
{
    if (!timeline_lock()) return 0;
    g_timeline.audio_epoch = epoch;
    g_timeline.audio_min_ms = target_ms < 0 ? 0 : target_ms;
    g_timeline.seek_pending = 1U;
    g_timeline.stats.audio_valid = 0;
    g_timeline.stats.audio_ms = g_timeline.audio_min_ms;
    g_timeline.audio_wall_us = session_now();
    g_timeline.audio_duration_us = 0;
    g_timeline.anchor_valid = 0;
    g_timeline.last_video_ms = -1;
    timeline_unlock();
    return 1;
}
int ppa_session_seek_arrived(unsigned int epoch, int video_ms)
{
    int arrived = 0;
    if (!timeline_lock()) return 0;
    if (g_timeline.audio_epoch == epoch) {
        arrived = 1;
        g_timeline.seek_pending = 0U;
        g_timeline.stats.audio_ms = video_ms;
        g_timeline.stats.audio_valid = 0;
        g_timeline.stats.video_ms = video_ms;
        g_timeline.anchor_ms = video_ms;
        g_timeline.anchor_wall_us = session_now();
        g_timeline.anchor_valid = 1;
        g_timeline.last_video_ms = video_ms;
    }
    timeline_unlock();
    return arrived;
}
static int audio_epoch_admissible_locked(unsigned int epoch, int64_t pts_ms)
{
    return !g_timeline.seek_pending &&
           (!g_timeline.audio_epoch || epoch == g_timeline.audio_epoch) &&
           pts_ms >= g_timeline.audio_min_ms;
}

int ppa_session_audio_output_epoch(unsigned int epoch, int64_t pts_ms,
                                   unsigned int sample_count,
                                   unsigned int sample_rate,
                                   int volume, void *data)
{
    struct ppa_bus_token audio_bus;
    uint64_t duration_us, remaining_us, now;
    int output_result, remaining;
    if (data == 0 || sample_count == 0U || sample_rate == 0U)
        return -1;
    if (!timeline_lock()) return 0;
    if (!audio_epoch_admissible_locked(epoch, pts_ms)) {
        timeline_unlock();
        return 0;
    }
    timeline_unlock();

    /* PSPSDK: OutputBlocking queues PCM and may wait for the previous block;
     * RestLength is the count of still-unplayed samples on the channel.
     * Publishing this block's PTS before that wait made audio appear up to a
     * complete block early. Publish its end minus remaining hardware samples
     * only after acceptance, including any previous queued tail. No timeline
     * semaphore is held across firmware output/wait calls. */
    audio_bus = ppa_bus_begin(PPA_BUS_AUDIO_OUTPUT_WAIT);
    output_result = sceAudioOutputBlocking(0, volume, data);
    ppa_bus_end(&audio_bus);
    if (output_result < 0) return -1;
    remaining = sceAudioGetChannelRestLength(0);
    now = session_now();
    duration_us = (uint64_t)sample_count * 1000000ULL / sample_rate;
    /* If the optional position query fails, conservatively treat the accepted
     * block as just starting. The output error itself is never hidden. */
    remaining_us = remaining < 0 ? duration_us :
                   (uint64_t)(unsigned int)remaining * 1000000ULL / sample_rate;
    if (!timeline_lock()) return 1;
    /* A seek may have begun while OutputBlocking slept. Old PCM may finish in
     * hardware, but it cannot resurrect the previous epoch's clock. */
    if (!audio_epoch_admissible_locked(epoch, pts_ms)) {
        timeline_unlock();
        return 0;
    }
    g_timeline.stats.audio_ms = pts_ms + (int64_t)(duration_us / 1000ULL) -
                              (int64_t)(remaining_us / 1000ULL);
    g_timeline.stats.audio_valid = 1;
    g_timeline.audio_wall_us = now;
    g_timeline.audio_duration_us = remaining_us;
    timeline_unlock();
    return 1;
}
int ppa_session_audio_time_ms(int fallback_ms)
{
    int64_t pts = fallback_ms;
    if (timeline_lock()) {
        if (g_timeline.stats.audio_valid) pts = audio_time_locked(session_now());
        timeline_unlock();
    }
    return pts > INT_MAX ? INT_MAX : (pts < INT_MIN ? INT_MIN : (int)pts);
}
uint64_t ppa_session_video_target_us(int64_t pts_ms)
{
    uint64_t now = session_now(), target = now;
    if (!timeline_lock()) return target;
    /* Audio is the master whenever a submitted block is known. A permanent
     * wall anchor chosen during seek preroll/arrival preserved the wrong
     * audio/video phase indefinitely, and could accumulate playback drift.
     * Keep the wall anchor solely for startup or genuinely absent audio. */
    if (g_timeline.stats.audio_valid && !g_timeline.seek_pending) {
        int64_t audio_ms = audio_time_locked(now);
        if (pts_ms > audio_ms)
            target = now + (uint64_t)(pts_ms - audio_ms) * 1000ULL;
        g_timeline.anchor_ms = audio_ms;
        g_timeline.anchor_wall_us = now;
        g_timeline.anchor_valid = 1;
        g_timeline.last_video_ms = pts_ms;
        timeline_unlock();
        return target;
    }
    if (!g_timeline.anchor_valid ||
        pts_ms < g_timeline.anchor_ms - 250 ||
        (g_timeline.last_video_ms >= 0 && pts_ms - g_timeline.last_video_ms > 10000)) {
        g_timeline.anchor_ms = g_timeline.stats.audio_valid ?
                                audio_time_locked(now) : pts_ms;
        g_timeline.anchor_wall_us = now;
        g_timeline.anchor_valid = 1;
    }
    if (pts_ms > g_timeline.anchor_ms)
        target = g_timeline.anchor_wall_us +
                 (uint64_t)(pts_ms - g_timeline.anchor_ms) * 1000ULL;
    else
        target = g_timeline.anchor_wall_us;
    g_timeline.last_video_ms = pts_ms;
    timeline_unlock();
    return target;
}
void ppa_session_note_present(int64_t pts_ms)
{
    if (!timeline_lock()) return;
    g_timeline.stats.video_ms = pts_ms;
    g_timeline.stats.presented++;
    timeline_unlock();
}
void ppa_session_note_decode(unsigned int elapsed_us, int duration_ms)
{
    if (!g_options.health_panel || !timeline_try_lock()) return;
    g_timeline.stats.decode_us = elapsed_us;
    if (duration_ms > 0) g_timeline.stats.budget_us = (unsigned int)duration_ms * 1000U;
    timeline_unlock();
}
void ppa_session_note_skip(void)
{
    if (!g_options.health_panel || !timeline_try_lock()) return;
    g_timeline.stats.skipped++;
    timeline_unlock();
}
void ppa_session_note_compat(unsigned int original_refs, unsigned int effective_refs,
                            int active, unsigned int weights_disabled)
{
    if (!g_options.health_panel || !timeline_try_lock()) return;
    g_timeline.stats.original_refs = original_refs;
    g_timeline.stats.effective_refs = effective_refs;
    g_timeline.stats.compatibility = active;
    g_timeline.stats.weights_disabled = weights_disabled;
    timeline_unlock();
}
int ppa_session_snapshot(struct ppa_session_snapshot *out)
{
    if (!out || !timeline_try_lock()) return 0;
    *out = g_timeline.stats;
    if (out->audio_valid) out->audio_ms = audio_time_locked(session_now());
    timeline_unlock();
    return 1;
}

void ppa_session_audio_only_consumed(void)
{ __sync_fetch_and_sub(&g_audio_pending, 1U); }
unsigned int ppa_session_audio_only_queued_blocks(void)
{ return atomic_read(&g_audio_pending); }
int ppa_session_audio_publish(SceUID can_get)
{
    int result;
    __sync_fetch_and_add(&g_audio_pending, 1U);
    result = sceKernelSignalSema(can_get, 1);
    if (result < 0) __sync_fetch_and_sub(&g_audio_pending, 1U);
    return result;
}

int ppa_session_run_audio_only(struct ppa_audio_only_adapter *a)
{
    unsigned int pending_service_us = 0U, pending_codec_us = 0U;
    while (!*a->stop) {
        char *error = 0;
        unsigned int start_us, elapsed, codec_us = 0U;
        int wait;
        if (!ppa_session_pause_point(a->stop, a->paused, PPA_SESSION_WORKER_DECODE)) break;
        wait = ppa_wait_sema_poll(a->stop, a->can_put,
                    "audio-only: producer wait failed", &error, PPA_SEMA_WAIT_TIMEOUT_US);
        if (wait < 0) {
            if (error) *a->result = error;
            *a->stop = 1;
            break;
        }
        if (wait == 0) continue;
        start_us = sceKernelGetSystemTimeLow();
        error = a->decode(a->owner, &codec_us);
        elapsed = sceKernelGetSystemTimeLow() - start_us;
        /* A bounded read can retain partial PCM and ask to retry. Charge all
         * attempts to the eventual output block, excluding semaphore/pause
         * waits, so the governor cannot downshift on an incomplete cost. */
        pending_service_us = elapsed > UINT_MAX - pending_service_us ?
            UINT_MAX : pending_service_us + elapsed;
        pending_codec_us = codec_us > UINT_MAX - pending_codec_us ?
            UINT_MAX : pending_codec_us + codec_us;
        if (error) {
            if (sceKernelSignalSema(a->can_put, 1) < 0) {
                *a->result = "audio-only: credit release failed"; *a->stop = 1; break;
            }
            if (!strcmp(error, PPU_AUDIO_AGAIN)) {
                /* Return to the scheduler before retrying: storage runs below
                 * this producer's priority. Keep the credit and PCM ownership
                 * rules above, and recheck stop/pause on the next iteration. */
                sceKernelDelayThread(1U);
                continue;
            }
            if (!a->at_eof(a->owner, error)) { *a->result = error; *a->stop = 1; break; }
            /* EOF is published only after actual output has consumed all queued
             * blocks; no synthetic silence or fixed-duration tail delay. */
            while (!*a->stop && atomic_read(&g_audio_pending) != 0U) {
                if (!ppa_session_pause_point(a->stop, a->paused, PPA_SESSION_WORKER_DECODE)) break;
                sceKernelDelayThread(5000U);
            }
            /* OutputBlocking can return after submitting the last block.
             * Its DMA-visible ring slot stays alive until the channel drains. */
            while (!*a->stop && sceAudioGetChannelRestLength(0) > 0) {
                if (!ppa_session_pause_point(a->stop, a->paused, PPA_SESSION_WORKER_DECODE)) break;
                sceKernelDelayThread(1000U);
            }
            if (!*a->stop) *a->eof = 1;
            break;
        }
        /* The governor producer also publishes health when auto clock is off;
         * publish once per completed PCM block, including its retry work. */
        if (!a->drain_only)
            cpu_clock_auto_on_audio_only_produced_us(pending_service_us,
                pending_codec_us, a->duration_ms);
        pending_service_us = pending_codec_us = 0U;
        if (ppa_session_audio_publish(a->can_get) < 0) {
            *a->result = "audio-only: output signal failed";
            *a->stop = 1;
            break;
        }
        a->progress(a->owner);
    }
    return 0;
}
