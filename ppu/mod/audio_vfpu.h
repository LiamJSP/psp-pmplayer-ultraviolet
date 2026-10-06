#ifndef PPA_AUDIO_VFPU_H
#define PPA_AUDIO_VFPU_H

#ifdef __cplusplus
extern "C" {
#endif

/* Phase-4 audio acceleration lifecycle. The session is active only for a real
 * stereo sample-rate conversion. Scalar remains the initial/authoritative
 * implementation; a dedicated VFPU worker is admitted only after measured
 * end-to-end savings (including scheduling and VFPU context overhead). */
void ppa_audio_accel_session_begin(unsigned int input_frames,
                                   unsigned int output_frames,
                                   int resample_required);
void ppa_audio_accel_session_end(void);

/* Returns non-zero when the session handled the complete resample. A zero
 * return means the caller must run its generic scalar fallback. */
int ppa_audio_accel_resample(short *dest_pcm_buffer,
                             const short *src_pcm_buffer,
                             unsigned int input_frames,
                             unsigned int output_frames);

#ifdef __cplusplus
}
#endif

#endif
