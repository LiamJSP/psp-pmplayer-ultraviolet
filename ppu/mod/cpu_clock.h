#ifndef cpu_clock_h
#define cpu_clock_h

#include <stdint.h>

struct speed_setting_struct {
	int cpu;
	int ram;
	int bus;
};

#ifdef __cplusplus
extern "C" {
#endif

void cpu_clock_set_cpu_speed(int cpu);
void cpu_clock_set_speed(struct speed_setting_struct *speed);
void cpu_clock_set_maximum(void);
void cpu_clock_set_minimum(void);
void cpu_clock_enter_playback(void);
void cpu_clock_leave_playback(void);

void cpu_clock_set_auto_enabled(int enabled);
int  cpu_clock_get_auto_enabled(void);
void cpu_clock_set_extreme_battery_saver(int enabled);
int  cpu_clock_get_extreme_battery_saver(void);
int  cpu_clock_get_current_cpu(void);

/* Optional visual work is shed briefly after playback misses its budget. */
int cpu_clock_auto_optional_work_allowed(void);

uint64_t cpu_clock_auto_now_us(void);

typedef enum cpu_clock_auto_boost_reason {
	CPU_CLOCK_BOOST_GENERIC = 0,
	CPU_CLOCK_BOOST_OPEN,
	CPU_CLOCK_BOOST_RESUME,
	CPU_CLOCK_BOOST_SEEK,
	CPU_CLOCK_BOOST_RESET,
	CPU_CLOCK_BOOST_SUBTITLE,
	CPU_CLOCK_BOOST_VISUAL_CHANGE,
	CPU_CLOCK_BOOST_AUDIO_RECONFIG,
	CPU_CLOCK_BOOST_OUTPUT_MODE,
	CPU_CLOCK_BOOST_TRICKPLAY_ENTER,
	CPU_CLOCK_BOOST_TRICKPLAY_LEAVE
} cpu_clock_auto_boost_reason;

void cpu_clock_auto_reset(void);
void cpu_clock_auto_boost(void);
void cpu_clock_auto_boost_for_reason(cpu_clock_auto_boost_reason reason);
void cpu_clock_auto_pause(void);
void cpu_clock_auto_finish_session(void);

/* Called only after ppa_frame_sink_display_timed() completes successfully. */
void cpu_clock_auto_on_present_complete(int audio_timestamp_ms,
                                        int video_timestamp_ms,
                                        int video_frame_duration_ms);

/* Compatibility wrapper for external modules; new call sites must use the
 * completed-presentation name so pre-submit accounting is not reintroduced. */
void cpu_clock_auto_on_displayed_frame(int audio_timestamp_ms,
                                       int video_timestamp_ms,
                                       int video_frame_duration_ms);

/* Audio-only uses the same clock owner with an audio-block budget. No fake
 * video presentations or A/V drift observations are emitted. */
void cpu_clock_auto_on_audio_only_complete(int block_duration_ms);
/* One producer-owned pair: whole reader/decode service time, and its native
 * codec/PCM part. Storage bursts are excluded from the scalable codec peak;
 * mean service time still constrains refill headroom at the next CPU/bus tier. */
void cpu_clock_auto_on_audio_only_produced_us(unsigned int total_us,
                                             unsigned int codec_us,
                                             int block_duration_ms);
void cpu_clock_auto_on_frame_skipped(void);

void cpu_clock_auto_on_decode_us(unsigned int elapsed_us,
                                 int video_frame_duration_ms);

void cpu_clock_auto_on_gu_draw_us(unsigned int elapsed_us);
void cpu_clock_auto_on_postprocess_us(unsigned int elapsed_us);
/* Producer-side stereo resampling is measured separately so the downshift
 * model accounts for real audio duty without treating every audio block as a
 * complete video decode frame. */
void cpu_clock_auto_on_audio_resample_us(unsigned int elapsed_us);

#ifdef __cplusplus
}
#endif

#endif
