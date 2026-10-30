#include "ppa_playback_control.h"
#include "ppa_debug.h"
#include "ppa_playback_session.h"
#include "../mod/cpu_clock.h"

/* Playback events do not own a second governor.  They publish a typed workload
 * transition to the unified clock owner, which applies a bounded hold without a
 * polling thread. */
static void ppa_playback_control_boost(cpu_clock_auto_boost_reason reason)
{
    if (cpu_clock_get_auto_enabled())
        cpu_clock_auto_boost_for_reason(reason);
}

void ppa_playback_control_pause(void)
{
    ppa_debug_event("pause requested");
    ppa_session_set_paused(1);
    /* The controller lowers clocks only after every worker acknowledges pause. */
}

void ppa_playback_control_resume(void)
{
    ppa_debug_event("resume requested");
    ppa_playback_control_boost(CPU_CLOCK_BOOST_RESUME);
    ppa_session_set_paused(0);
}

void ppa_playback_control_seek(int direction_or_timestamp)
{
    ppa_session_discontinuity();
    ppa_playback_control_boost(CPU_CLOCK_BOOST_SEEK);
}

void ppa_playback_control_audio_offset(int offset_ms)
{
    ppa_playback_control_boost(CPU_CLOCK_BOOST_AUDIO_RECONFIG);
}

void ppa_playback_control_reset(void)
{
    ppa_session_discontinuity();
    ppa_playback_control_boost(CPU_CLOCK_BOOST_RESET);
}

void ppa_playback_control_subtitle_change(int subtitle_id)
{
    ppa_playback_control_boost(CPU_CLOCK_BOOST_SUBTITLE);
}

void ppa_playback_control_tvout_change(int mode)
{
    ppa_playback_control_boost(CPU_CLOCK_BOOST_OUTPUT_MODE);
}

void ppa_playback_control_aspect_change(int aspect)
{
    ppa_playback_control_boost(CPU_CLOCK_BOOST_VISUAL_CHANGE);
}

void ppa_playback_control_zoom_change(int zoom)
{
    ppa_playback_control_boost(CPU_CLOCK_BOOST_VISUAL_CHANGE);
}

void ppa_playback_control_luminosity_change(int level)
{
    ppa_playback_control_boost(CPU_CLOCK_BOOST_VISUAL_CHANGE);
}

void ppa_playback_control_audio_track_change(int track)
{
    ppa_playback_control_boost(CPU_CLOCK_BOOST_AUDIO_RECONFIG);
}

void ppa_playback_control_audio_channel_change(int channel)
{
    ppa_playback_control_boost(CPU_CLOCK_BOOST_AUDIO_RECONFIG);
}

void ppa_playback_control_postfx_change(int mode)
{
    ppa_playback_control_boost(CPU_CLOCK_BOOST_VISUAL_CHANGE);
}

void ppa_playback_control_subtitle_style_change(int style_value)
{
    ppa_playback_control_boost(CPU_CLOCK_BOOST_SUBTITLE);
}

void ppa_playback_control_trickplay_enter(int direction_or_level)
{
    ppa_playback_control_boost(CPU_CLOCK_BOOST_TRICKPLAY_ENTER);
}

void ppa_playback_control_trickplay_leave(int final_timestamp_ms)
{
    ppa_playback_control_boost(CPU_CLOCK_BOOST_TRICKPLAY_LEAVE);
}

void ppa_playback_control_output_mode_change(int mode)
{
    ppa_playback_control_boost(CPU_CLOCK_BOOST_OUTPUT_MODE);
}

/* Publish the run flag last: a decoder waking at the old pause clock must not
 * enter Sony firmware before the resume boost has completed. */
void ppa_playback_control_unpause(volatile int *paused)
{
    if (*paused) ppa_playback_control_resume();
    else ppa_session_set_paused(0);
    __sync_synchronize();
    *paused = 0;
}

void ppa_playback_control_toggle_pause(volatile int *paused)
{
    if (*paused) ppa_playback_control_unpause(paused);
    else {
        *paused = 1;
        ppa_playback_control_pause();
    }
}
