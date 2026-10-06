/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PPU_AUDIO_PCM_VFPU_H
#define PPU_AUDIO_PCM_VFPU_H

/* Private streaming-PCM boundary, not a replacement for libsamplerate's
 * public helpers. When enabled, callers MUST own a VFPU-enabled PSP thread.
 * M000/M100/M200 and prefix registers are call-clobbered; nothing is retained
 * between calls. Buffers must not overlap. Counts are interleaved samples,
 * not stereo frames. Finite SRC output is the float-input contract.
 * Alignment and short tails are handled internally without overreading.
 */
int ppu_audio_pcm_vfpu_enabled(void);
void ppu_audio_pcm_to_float(const short *in, float *out, int samples);
void ppu_audio_float_to_pcm(const float *in, short *out, int samples);

#endif
