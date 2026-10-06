#ifndef PPA_VIDEO_PIPELINE_C_H
#define PPA_VIDEO_PIPELINE_C_H

#include <stdint.h>

#define PPA_BATTERY_SAVER_REJECTED "ppa: extreme battery saver incompatible media"

#ifdef __cplusplus
extern "C" {
#endif

void ppa_video_pipeline_reload_config(void);
void ppa_video_pipeline_reset(void);
void ppa_video_pipeline_leave_playback(void);
void ppa_video_pipeline_shutdown(void);
void ppa_video_pipeline_set_lcd_output(int lcd_output);
void ppa_video_pipeline_set_avc_sps(const unsigned char *sps,
                                    unsigned int size);
/* Stable configuration intent used while choosing frame-buffer topology.
 * It remains distinct from enabled() so output-specific applicability and an
 * explicit Extreme Battery Saver policy can be reported cleanly. */
int ppa_video_pipeline_configured(void);
int ppa_video_pipeline_enabled(void);
/* True only when the next playback presentation thread can execute a VFPU
 * panel color stage. Callers use this before thread creation so
 * ordinary/native playback does not pay VFPU context save/restore cost. */
int ppa_video_pipeline_vfpu_required(void);

/* Fully gated low-power mode. When enabled, all post-processing stages are
 * bypassed and container open paths admit only native-compatible media. */
int ppa_video_pipeline_extreme_battery_saver_enabled(void);

/* Trick-play may intentionally discard intermediate decoded pictures. This is
 * separate from ordinary A/V-sync dropping so the governor does not interpret
 * the discard as a missed playback target. */
void ppa_video_pipeline_set_trickplay_frame_drop(int enabled);
int ppa_video_pipeline_trickplay_frame_drop_allowed(void);
int ppa_video_pipeline_should_drop_trickplay_frame(unsigned int level,
                                                    unsigned int sequence);

int ppa_video_pipeline_process(void *frame_buffer,
                               unsigned int output_pitch,
                               unsigned int source_width,
                               unsigned int source_height,
                               unsigned int aspect_ratio,
                               unsigned int zoom,
                               unsigned int frame_number);

#ifdef __cplusplus
}
#endif

#endif
