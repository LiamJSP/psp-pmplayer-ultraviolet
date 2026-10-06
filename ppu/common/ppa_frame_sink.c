#include "ppa_frame_sink.h"
#include "ppa_playback_session.h"
#include "ppa_cache.h"
#include "ppa_bus_manager.h"

#include <pspdisplay.h>
#include <pspthreadman.h>
#include <string.h>

#define PPA_PACER_COARSE_MARGIN_US 1400ULL
#define PPA_PACER_DISCONTINUITY_MS 10000LL
#define PPA_PACER_BACKWARD_MS       250LL
#define PPA_PACER_MIN_INTERVAL_MS     14
#define PPA_PACER_MAX_INTERVAL_MS   250

struct ppa_frame_pacer_state {
    int64_t last_media_ms;
    int effective_interval_us;
};

static struct ppa_frame_pacer_state g_pacer;
static int64_t g_synthetic_timestamp_ms;
static int g_last_frame_late;

int ppa_frame_surface_equal(const void *a, const void *b)
{
    return a && b && ppa_cached_cptr(a) == ppa_cached_cptr(b);
}
void ppa_frame_lease_init(struct ppa_frame_lease *lease)
{ memset(lease, 0, sizeof(*lease)); }
void ppa_frame_lease_skip(struct ppa_frame_lease *lease)
{ ++lease->retained_credits; }
int ppa_frame_lease_complete_swap(struct ppa_frame_lease *lease,
                                 SceUID credits, const void *scanout,
                                 unsigned int epoch, int64_t timestamp_ms)
{
    int result = lease->retained_credits ?
        sceKernelSignalSema(credits, (int)lease->retained_credits) : 0;
    if (result < 0) return result;
    lease->retained_credits = 1U;
    lease->scanout = scanout;
    lease->epoch = epoch;
    lease->timestamp_ms = timestamp_ms;
    return 0;
}

static uint64_t ppa_frame_sink_now_us(void)
{
    return (uint64_t)sceKernelGetSystemTimeWide();
}

void ppa_frame_sink_reset_timing(void)
{
    memset(&g_pacer, 0, sizeof(g_pacer));
    g_pacer.last_media_ms = -1;
    g_synthetic_timestamp_ms = 0;
    ppa_session_discontinuity();
}

int ppa_frame_sink_effective_duration_ms(int64_t previous_timestamp_ms,
                                         int64_t timestamp_ms,
                                         int nominal_duration_ms)
{
    int64_t delta = timestamp_ms - previous_timestamp_ms;

    if (delta >= PPA_PACER_MIN_INTERVAL_MS &&
        delta <= PPA_PACER_MAX_INTERVAL_MS)
        return (int)delta;

    if (nominal_duration_ms < PPA_PACER_MIN_INTERVAL_MS)
        nominal_duration_ms = 17;
    if (nominal_duration_ms > PPA_PACER_MAX_INTERVAL_MS)
        nominal_duration_ms = PPA_PACER_MAX_INTERVAL_MS;
    return nominal_duration_ms;
}

static void ppa_frame_sink_observe_interval(int64_t media_timestamp_ms,
                                            int nominal_duration_ms)
{
    int interval_ms;
    int interval_us;

    if (g_pacer.last_media_ms < 0) {
        g_pacer.last_media_ms = media_timestamp_ms;
        g_pacer.effective_interval_us = nominal_duration_ms * 1000;
        return;
    }

    interval_ms = ppa_frame_sink_effective_duration_ms(g_pacer.last_media_ms,
                                                        media_timestamp_ms,
                                                        nominal_duration_ms);
    interval_us = interval_ms * 1000;

    if (g_pacer.effective_interval_us <= 0)
        g_pacer.effective_interval_us = interval_us;
    else
        g_pacer.effective_interval_us =
            (g_pacer.effective_interval_us * 7 + interval_us) / 8;

    g_pacer.last_media_ms = media_timestamp_ms;
}

static void ppa_frame_sink_wait_for_pts(int64_t media_timestamp_ms,
                                        int nominal_duration_ms)
{
    uint64_t now_us = ppa_frame_sink_now_us();
    uint64_t target_us = ppa_session_video_target_us(media_timestamp_ms);
    g_last_frame_late = 0;
    if (target_us > now_us + PPA_PACER_COARSE_MARGIN_US) {
        uint64_t delay = target_us - now_us - PPA_PACER_COARSE_MARGIN_US;
        /* Bound a malformed timestamp's wait so stop/pause can still progress. */
        if (delay > 250000ULL) delay = 250000ULL;
        sceKernelDelayThreadCB((unsigned int)delay);
    } else if (now_us > target_us + 17000ULL) {
        g_last_frame_late = 1;
    }
    ppa_frame_sink_observe_interval(media_timestamp_ms, nominal_duration_ms);
}

int ppa_frame_sink_display_timed(void *frame_buffer,
                                 int texture_width,
                                 int pixel_format,
                                 int setbuf_mode,
                                 int64_t media_timestamp_ms,
                                 int nominal_duration_ms)
{
    struct ppa_bus_token display_bus;
    unsigned int frame_budget_us;

    int setbuf_result;
    int vblank_result;

    if (frame_buffer == 0 || texture_width <= 0)
        return -1;

    ppa_frame_sink_wait_for_pts(media_timestamp_ms, nominal_duration_ms);
    display_bus = ppa_bus_begin(PPA_BUS_DISPLAY);

    /* Queue the swap for the next vertical boundary, then wait for that
     * boundary to complete before the caller releases the previous ring slot.
     * The old wait-then-IMMEDIATE/NEXTFRAME ordering could either switch at an
     * hsync in the middle of scanout or return while the old buffer was still
     * being displayed, allowing the decoder to overwrite it and producing a
     * horizontal tear. Playback always uses the ownership-safe NEXTFRAME
     * transaction; setbuf_mode is retained in the ABI for older callers. */
    (void)setbuf_mode;
    setbuf_result = sceDisplaySetFrameBuf(frame_buffer,
                                          texture_width,
                                          pixel_format,
                                          PSP_DISPLAY_SETBUF_NEXTFRAME);
    if (setbuf_result < 0) {
        ppa_bus_end(&display_bus);
        return setbuf_result;
    }

    vblank_result = sceDisplayWaitVblankStart();
    ppa_bus_end(&display_bus);
    if (vblank_result < 0) {
        return vblank_result;
    }

    ppa_session_note_present(media_timestamp_ms);
    frame_budget_us = g_pacer.effective_interval_us > 0 ?
                      (unsigned int)g_pacer.effective_interval_us :
                      (unsigned int)(nominal_duration_ms > 0 ? nominal_duration_ms : 17) * 1000U;
    ppa_bus_frame_pulse(frame_budget_us, g_last_frame_late);

    return 0;
}

int ppa_frame_sink_display(void *frame_buffer,
                           int texture_width,
                           int pixel_format,
                           int setbuf_mode)
{
    int result = ppa_frame_sink_display_timed(frame_buffer,
                                               texture_width,
                                               pixel_format,
                                               setbuf_mode,
                                               g_synthetic_timestamp_ms,
                                               17);
    g_synthetic_timestamp_ms += 17;
    return result;
}
