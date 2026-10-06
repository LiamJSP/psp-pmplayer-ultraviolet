#ifndef PPU_AUDIO_STREAM_H
#define PPU_AUDIO_STREAM_H
#include <stdint.h>
#include "audio_format.h"
#include "common/ppa_media_packet.h"

#define PPU_AUDIO_EOF "audio: end of stream"
#define PPU_AUDIO_AGAIN "audio: yield"

struct ppu_audio_stream;
typedef char *(*ppu_audio_read_packet)(void *reader, unsigned int track,
                                      struct ppa_media_packet *packet);
struct ppu_audio_stream *ppu_audio_stream_open(const struct ppu_audio_format *format);
void ppu_audio_stream_close(struct ppu_audio_stream *stream);
int ppu_audio_stream_reset(struct ppu_audio_stream *stream, int at_start);
/* Required PSP attributes for the thread that calls read. Query after open,
 * before thread creation. It may use VFPU when source-rate conversion is
 * active. Open/reset/close do not require VFPU; 44100-Hz bypass returns zero. */
unsigned int ppu_audio_stream_thread_attributes(const struct ppu_audio_stream *stream);
/* A successful call fills exactly PPU_AUDIO_BLOCK stereo frames. Partial
 * blocks/history survive backpressure. Only true EOF drains/zero-pads once.
 * Reader returns PPU_AUDIO_EOF only at a clean container boundary. */
char *ppu_audio_stream_read(struct ppu_audio_stream *stream,
                            ppu_audio_read_packet read_packet, void *reader,
                            unsigned int track, int16_t *out, int *timestamp,
                            unsigned int *work_us);
void ppu_audio_stream_silence(struct ppu_audio_stream *stream, int16_t *out,
                              int *timestamp);
#endif
