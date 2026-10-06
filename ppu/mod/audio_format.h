#ifndef PPU_AUDIO_FORMAT_H
#define PPU_AUDIO_FORMAT_H

#include <stdint.h>
#include "mp4info.h"
#include "mkvinfo.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PPU_AUDIO_AAC  0x6d703461U
#define PPU_AUDIO_MP3  0x6d703320U
#define PPU_AUDIO_FLAC 0x664c6143U
#define PPU_AUDIO_OPUS 0x4f707573U
#define PPU_AUDIO_RATE 44100U
#define PPU_AUDIO_BLOCK 1024U
#define PPU_AUDIO_MAX_DECODE_FRAMES 8192U
#define PPU_AUDIO_MAX_PACKET_BYTES (128U * 1024U)

/* Producer-owned, copied at open/track change. No borrowed metadata pointers. */
struct ppu_audio_format {
    uint32_t type, rate, channels, bits, max_frames;
    uint32_t start_skip, delay_samples, preroll_frames;
    int gain_q8;
    int mp4_timeline;
    uint64_t end_sample; /* exclusive source position; zero = no edit limit */
    uint8_t flac_header[42]; /* marker + last STREAMINFO, no optional metadata */
};

int ppu_audio_format_mp4(const mp4info_t *info, const mp4info_track_t *track,
                         struct ppu_audio_format *out);
int ppu_audio_format_mkv(const mkvinfo_track_t *track,
                         struct ppu_audio_format *out);
/* Frame count from a bounded packet header, before PCM allocation/decoding.
 * FLAC packets must contain one native FLAC frame; Opus one mapping-family-0
 * packet. The decoder still validates the complete compressed payload. */
int ppu_audio_packet_frames(uint32_t type, const uint8_t *data,
                            uint32_t size, uint32_t rate);

#ifdef __cplusplus
}
#endif
#endif
