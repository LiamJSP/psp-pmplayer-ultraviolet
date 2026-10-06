#include "audio_software.h"
#include "common/mem64.h"
#include <string.h>
#include <stdlib.h>
#ifndef PPU_ENABLE_OPUS
#define PPU_ENABLE_OPUS 1
#endif
#if PPU_ENABLE_OPUS
/* PSPDEV's `opus` package supplies this packet decoder. `opusfile` consumes
 * Ogg byte streams, not the already-demuxed MP4/MKV packets passed below.
 * opus_decode returns integer PCM with either upstream DSP configuration;
 * fixed-point selection belongs to the library build, not PPU's CFLAGS. */
#include <opus/opus.h>
#endif

/* This translation unit owns the one dr_flac implementation. Keep the CRC
 * checks; container packets provide all I/O, without stdio or Ogg machinery. */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#include "../../third_party/dr_libs/dr_flac.h"

#define PPU_FLAC_ARENA_BYTES (128U * 1024U)
#define PPU_FLAC_CONVERT_FRAMES 512U
#define PPU_FLAC_CONVERT_BYTES (PPU_FLAC_CONVERT_FRAMES * 2U * sizeof(drflac_int32))
#define PPU_FLAC_DECODER_BYTES (PPU_FLAC_ARENA_BYTES - PPU_FLAC_CONVERT_BYTES)
#define PPU_OPUS_STATE_LIMIT (128U * 1024U)

struct ppu_soft_audio {
    struct ppu_audio_format format;
    void *storage;
    unsigned int arena_used;
    const uint8_t *packet;
    size_t packet_bytes, cursor;
};

static void *flac_alloc(size_t bytes, void *user)
{
    struct ppu_soft_audio *d = user;
    unsigned int offset = (d->arena_used + 63U) & ~63U;
    if (offset > PPU_FLAC_DECODER_BYTES || bytes > PPU_FLAC_DECODER_BYTES - offset) return 0;
    d->arena_used = offset + (unsigned int)bytes;
    return (uint8_t *)d->storage + offset;
}
static void flac_free(void *ptr, void *user) { (void)ptr; (void)user; }
/* drflac_open without metadata callbacks uses a single decoder allocation.
 * Any upstream fallback allocation remains confined to this bounded arena. */
static size_t flac_read(void *user, void *out, size_t bytes)
{
    struct ppu_soft_audio *d = user;
    size_t done = 0, available = 42 + d->packet_bytes - d->cursor;
    if (bytes > available) bytes = available;
    if (d->cursor < 42) {
        done = 42 - d->cursor;
        if (done > bytes) done = bytes;
        memcpy(out, d->format.flac_header + d->cursor, done);
        d->cursor += done;
    }
    if (bytes > done) {
        memcpy((uint8_t *)out + done, d->packet + d->cursor - 42, bytes - done);
        d->cursor += bytes - done;
    }
    return bytes;
}
static drflac_bool32 flac_seek(void *user, int offset, drflac_seek_origin origin)
{
    struct ppu_soft_audio *d = user;
    size_t base;
    if (offset < 0) return DRFLAC_FALSE;
    if (origin == DRFLAC_SEEK_SET) base = 0;
    else if (origin == DRFLAC_SEEK_CUR) base = d->cursor;
    else return DRFLAC_FALSE;
    if ((size_t)offset > 42 + d->packet_bytes - base) return DRFLAC_FALSE;
    d->cursor = base + (size_t)offset;
    return DRFLAC_TRUE;
}
static drflac_bool32 flac_tell(void *user, drflac_int64 *cursor)
{ *cursor = ((struct ppu_soft_audio *)user)->cursor; return DRFLAC_TRUE; }

/* dr_flac's s32 API returns left-aligned samples. Round once to the nearest
 * s16 value (halfway towards +infinity), then saturate at the positive rail.
 * Bias into unsigned space first: no signed overflow, negative shifts, float,
 * per-sample division, dither state or noise-shaping workload is needed. */
static int16_t flac_round_s16(drflac_int32 sample)
{
    uint32_t biased = (uint32_t)sample + 0x80000000U;
    uint32_t rounded = (biased >> 16) + ((biased & 0xffffU) >= 0x8000U);
    if (rounded > 65535U) rounded = 65535U;
    return (int16_t)((int32_t)rounded - 32768);
}

static int flac_read_s16(struct ppu_soft_audio *d, drflac *flac,
                          int16_t *pcm, unsigned int expected)
{
    unsigned int done = 0;
    drflac_int32 *wide = (drflac_int32 *)((uint8_t *)d->storage + PPU_FLAC_DECODER_BYTES);
    if (d->format.bits <= 16U)
        return (int)drflac_read_pcm_frames_s16(flac, expected, pcm);
    /* The final 4 KiB of the existing arena is a reusable conversion tile,
     * disjoint from decoder allocations. Never put a whole wide frame on the
     * 64-KiB producer stack or expand the real-time heap. */
    while (done < expected) {
        unsigned int count = expected - done, got, i;
        if (count > PPU_FLAC_CONVERT_FRAMES) count = PPU_FLAC_CONVERT_FRAMES;
        got = (unsigned int)drflac_read_pcm_frames_s32(flac, count, wide);
        if (got > count) return -1;
        for (i = 0; i < got * d->format.channels; ++i)
            pcm[done * d->format.channels + i] = flac_round_s16(wide[i]);
        done += got;
        if (got != count) break;
    }
    return (int)done;
}

int ppu_soft_audio_reset(struct ppu_soft_audio *d)
{
    if (!d) return 0;
    if (d->format.type == PPU_AUDIO_FLAC) return 1;
#if PPU_ENABLE_OPUS
    if (d->format.type == PPU_AUDIO_OPUS)
        return opus_decoder_init((OpusDecoder *)d->storage, 48000, d->format.channels) == OPUS_OK &&
               opus_decoder_ctl((OpusDecoder *)d->storage, OPUS_SET_GAIN(d->format.gain_q8)) == OPUS_OK;
#endif
    return 0;
}
struct ppu_soft_audio *ppu_soft_audio_open(const struct ppu_audio_format *f)
{
    struct ppu_soft_audio *d;
    unsigned int bytes = 0;
    if (!f) return 0;
    if (f->type == PPU_AUDIO_FLAC) bytes = PPU_FLAC_ARENA_BYTES;
#if PPU_ENABLE_OPUS
    else if (f->type == PPU_AUDIO_OPUS) {
        int size = opus_decoder_get_size(f->channels);
        if (size <= 0 || (unsigned int)size > PPU_OPUS_STATE_LIMIT) return 0;
        bytes = (unsigned int)size;
    }
#endif
    if (!bytes) return 0;
    d = calloc(1, sizeof(*d));
    if (!d) return 0;
    d->format = *f;
    d->storage = malloc_64(bytes);
    if (!d->storage || !ppu_soft_audio_reset(d)) {
        ppu_soft_audio_close(d); return 0;
    }
    return d;
}
void ppu_soft_audio_close(struct ppu_soft_audio *d)
{
    if (!d) return;
    if (d->storage) free_64(d->storage);
    free(d);
}
int ppu_soft_audio_decode(struct ppu_soft_audio *d, const uint8_t *packet,
                          unsigned int bytes, int16_t *pcm, unsigned int capacity)
{
    int expected, frames = -1;
    if (!d || !packet || !pcm) return -1;
    expected = ppu_audio_packet_frames(d->format.type, packet, bytes, d->format.rate);
    if (expected <= 0 || (unsigned int)expected > capacity ||
        (unsigned int)expected > d->format.max_frames) return -1;
    if (d->format.type == PPU_AUDIO_FLAC) {
        drflac_allocation_callbacks alloc;
        drflac *flac;
        drflac_int16 extra[2];
        d->arena_used = 0; d->packet = packet; d->packet_bytes = bytes; d->cursor = 0;
        memset(&alloc, 0, sizeof(alloc));
        alloc.pUserData = d; alloc.onMalloc = flac_alloc; alloc.onFree = flac_free;
        flac = drflac_open(flac_read, flac_seek, flac_tell, d, &alloc);
        if (flac) {
            if (flac->sampleRate == d->format.rate && flac->channels == d->format.channels &&
                flac->bitsPerSample == d->format.bits) {
                frames = flac_read_s16(d, flac, pcm, (unsigned int)expected);
                /* A packet must not hide a second frame or a short first one. */
                if (frames != expected || drflac_read_pcm_frames_s16(flac, 1, extra) != 0) frames = -1;
            }
            drflac_close(flac);
        }
        d->packet = 0; d->packet_bytes = d->cursor = 0;
    }
#if PPU_ENABLE_OPUS
    else if (d->format.type == PPU_AUDIO_OPUS)
        /* Keep integer output irrespective of a container's nominal bit-depth
         * tag. Passing the validated duration also bounds any library scratch
         * whose size depends on the requested output capacity. */
        frames = opus_decode((OpusDecoder *)d->storage, packet, (opus_int32)bytes,
                              pcm, expected, 0);
#endif
    if (frames != expected) return -1;
    if (d->format.channels == 1)
        for (int i = frames - 1; i >= 0; --i) pcm[2*i] = pcm[2*i+1] = pcm[i];
    return frames;
}
