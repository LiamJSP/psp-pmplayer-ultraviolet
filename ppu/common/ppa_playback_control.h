#ifndef PPA_PLAYBACK_CONTROL_H
#define PPA_PLAYBACK_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

void ppa_playback_control_pause(void);
void ppa_playback_control_resume(void);
void ppa_playback_control_seek(int direction_or_timestamp);
void ppa_playback_control_audio_offset(int offset_ms);
void ppa_playback_control_reset(void);
void ppa_playback_control_subtitle_change(int subtitle_id);
void ppa_playback_control_tvout_change(int mode);

/* Typed workload changes let the governor apply a proportional settle hold and
 * invalidate calibration without treating every menu action as a decoder reset. */
void ppa_playback_control_aspect_change(int aspect);
void ppa_playback_control_zoom_change(int zoom);
void ppa_playback_control_luminosity_change(int level);
void ppa_playback_control_audio_track_change(int track);
void ppa_playback_control_audio_channel_change(int channel);
void ppa_playback_control_postfx_change(int mode);
void ppa_playback_control_subtitle_style_change(int style_value);
void ppa_playback_control_trickplay_enter(int direction_or_level);
void ppa_playback_control_trickplay_leave(int final_timestamp_ms);
void ppa_playback_control_output_mode_change(int mode);

#ifdef __cplusplus
}
#endif

#endif
