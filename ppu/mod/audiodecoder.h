#ifndef PPA_AUDIO_DECODER_H
#define PPA_AUDIO_DECODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSP_CODEC_AUDIO_MP3 0x1002
#define PSP_CODEC_AUDIO_AAC 0x1003

int audio_decoder_open(int type, int samplerate, int samplecount, int blockalign);
int audio_decoder_close(void);
/* Lifecycle-owner query, after producers have stopped. */
int audio_decoder_is_open(void);
/* pcm_frames is the container/header-validated duration, at most the native
 * codec workspace capacity. data must hold pcm_frames * 4 bytes of stereo s16. */
int audio_decoder_decode(void *data,
                         int *data_size,
                         uint8_t *buffer,
                         int buffer_size, unsigned int pcm_frames);

#ifdef __cplusplus
}
#endif

#endif
