#ifndef PPU_AUDIO_SOFTWARE_H
#define PPU_AUDIO_SOFTWARE_H
#include <stdint.h>
#include "audio_format.h"
struct ppu_soft_audio;
struct ppu_soft_audio *ppu_soft_audio_open(const struct ppu_audio_format *format);
void ppu_soft_audio_close(struct ppu_soft_audio *decoder);
int ppu_soft_audio_reset(struct ppu_soft_audio *decoder);
/* Always produces interleaved stereo s16, duplicating mono backwards in place.
 * Capacity is in stereo PCM frames. No packet pointer survives this call. */
int ppu_soft_audio_decode(struct ppu_soft_audio *decoder, const uint8_t *packet,
                          unsigned int bytes, int16_t *pcm, unsigned int capacity);
#endif
