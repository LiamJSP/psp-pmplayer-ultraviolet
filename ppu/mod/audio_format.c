#include "audio_format.h"
#include <string.h>
#include <limits.h>
#ifndef PPU_ENABLE_OPUS
#define PPU_ENABLE_OPUS 1
#endif
#if PPU_ENABLE_OPUS
#include <opus/opus.h>
#endif

static unsigned int be16(const uint8_t *p)
{ return ((unsigned int)p[0] << 8) | p[1]; }
static unsigned int le16(const uint8_t *p)
{ return p[0] | ((unsigned int)p[1] << 8); }

static int native_format(uint32_t type, uint32_t rate, uint32_t channels,
                          struct ppu_audio_format *f)
{
    memset(f, 0, sizeof(*f));
    f->type = type; f->rate = rate; f->channels = channels;
    f->bits = 16;
    if (channels != 1 && channels != 2) return 0;
    if (type != PPU_AUDIO_AAC && type != PPU_AUDIO_MP3) return 0;
    if (rate != 22050 && rate != 24000 && rate != 32000 &&
        rate != 44100 && rate != 48000) return 0;
    f->max_frames = type == PPU_AUDIO_MP3 ? 1152 : 1024;
    return 1;
}

static int flac_format(const uint8_t *p, uint32_t size,
                        uint32_t rate, uint32_t channels,
                        struct ppu_audio_format *f)
{
    const uint8_t *s;
    unsigned int min_block, max_block;
    if (!p || size < 42 || memcmp(p, "fLaC", 4) ||
        (p[4] & 0x7f) != 0 || p[5] || p[6] || p[7] != 34) return 0;
    s = p + 8;
    min_block = be16(s); max_block = be16(s + 2);
    memset(f, 0, sizeof(*f));
    f->type = PPU_AUDIO_FLAC;
    f->rate = ((uint32_t)s[10] << 12) | ((uint32_t)s[11] << 4) | (s[12] >> 4);
    f->channels = ((s[12] >> 1) & 7) + 1;
    f->bits = (((s[12] & 1) << 4) | (s[13] >> 4)) + 1;
    f->max_frames = max_block;
    if (f->rate < 8000 || f->rate > 48000 ||
        (f->channels != 1 && f->channels != 2) ||
        f->bits < 4 || f->bits > 24 || min_block < 16 ||
        max_block < min_block || max_block > PPU_AUDIO_MAX_DECODE_FRAMES ||
        f->rate != rate || f->channels != channels) return 0;
    memcpy(f->flac_header, p, 42);
    f->flac_header[4] = 0x80; /* only STREAMINFO reaches the packet decoder */
    /* Each container packet is an independent frame, with a bounded local
     * decoder arena. Its source frame number is not a local stream length. */
    f->flac_header[21] &= 0xf0;
    memset(f->flac_header + 22, 0, 20); /* unknown sample count / MD5 */
    return 1;
}

static int opus_format(const uint8_t *p, uint32_t size, int mp4,
                        uint32_t channels, struct ppu_audio_format *f)
{
#if PPU_ENABLE_OPUS
    unsigned int gain;
    memset(f, 0, sizeof(*f));
    if (mp4) {
        if (!p || size != 11 || p[0] != 0 || p[10] != 0) return 0;
        f->channels = p[1]; f->start_skip = be16(p + 2);
        gain = be16(p + 8);
    } else {
        if (!p || size != 19 || memcmp(p, "OpusHead", 8) ||
            p[8] != 1 || p[18] != 0) return 0;
        f->channels = p[9]; f->start_skip = le16(p + 10);
        gain = le16(p + 16);
    }
    if ((f->channels != 1 && f->channels != 2) || f->channels != channels) return 0;
    f->gain_q8 = gain >= 32768 ? (int)gain - 65536 : (int)gain;
    f->type = PPU_AUDIO_OPUS; f->rate = 48000; f->bits = 16;
    f->max_frames = 5760; /* 120 ms, independent of packet duration */
    f->preroll_frames = 3840;
    f->delay_samples = f->start_skip;
    return 1;
#else
    (void)p; (void)size; (void)mp4; (void)channels; (void)f;
    return 0;
#endif
}

int ppu_audio_format_mp4(const mp4info_t *info, const mp4info_track_t *t,
                         struct ppu_audio_format *f)
{
    uint64_t end;
    if (!info || !t || !f || t->type != MP4_TRACK_AUDIO) return 0;
    if (native_format(t->audio_type, t->samplerate, t->channels, f)) return 1;
    if (t->audio_type == PPU_AUDIO_FLAC) {
        if (!flac_format(t->audio_private, t->audio_private_size,
                         t->samplerate, t->channels, f)) return 0;
    } else if (t->audio_type == PPU_AUDIO_OPUS) {
        if (t->samplerate != 48000 ||
            !opus_format(t->audio_private, t->audio_private_size, 1, t->channels, f)) return 0;
        /* In MP4 elst, not dOps PreSkip, defines presentation trimming. */
        if (t->audio_edit_status != 1) return 0;
    } else return 0;
    if (t->time_scale != f->rate || t->audio_edit_status < 0) return 0;
    f->mp4_timeline = 1;
    f->start_skip = f->delay_samples = 0;
    if (t->audio_edit_status == 1) {
        if (info->time_scale <= 0 || t->audio_edit_start > UINT32_MAX ||
            t->audio_edit_duration > UINT64_MAX / f->rate) return 0;
        end = t->audio_edit_duration * f->rate / (uint32_t)info->time_scale;
        if (!end || end > INT64_MAX - t->audio_edit_start) return 0;
        f->start_skip = f->delay_samples = (uint32_t)t->audio_edit_start;
        f->end_sample = t->audio_edit_start + end;
    }
    return 1;
}

int ppu_audio_format_mkv(const mkvinfo_track_t *t, struct ppu_audio_format *f)
{
    uint64_t delay;
    if (!t || !f || t->type != MATROSKA_TRACK_AUDIO) return 0;
    if (native_format(t->audio_type, t->samplerate, t->channels, f)) return 1;
    if ((t->audio_type == PPU_AUDIO_FLAC || t->audio_type == PPU_AUDIO_OPUS) &&
        t->compress_setting_size) return 0;
    if (t->audio_type == PPU_AUDIO_FLAC) {
        if (t->codec_delay_ns || t->seek_preroll_ns) return 0;
        return flac_format(t->private_data, t->private_size,
                           t->samplerate, t->channels, f);
    }
    if (t->audio_type != PPU_AUDIO_OPUS ||
        !opus_format(t->private_data, t->private_size, 0, t->channels, f)) return 0;
    if (t->codec_delay_ns > 2000000000ULL || t->seek_preroll_ns > 1000000000ULL) return 0;
    delay = (t->codec_delay_ns * 48000ULL + 500000000ULL) / 1000000000ULL;
    if (delay != f->start_skip) return 0;
    if (t->seek_preroll_ns > 80000000ULL)
        f->preroll_frames = (uint32_t)((t->seek_preroll_ns * 48000ULL + 999999999ULL) / 1000000000ULL);
    return 1;
}

int ppu_audio_packet_frames(uint32_t type, const uint8_t *p,
                            uint32_t size, uint32_t rate)
{
    unsigned int code, pos, n;
    if (!p || !size || size > PPU_AUDIO_MAX_PACKET_BYTES) return -1;
    if (type == PPU_AUDIO_AAC) return 1024;
    if (type == PPU_AUDIO_MP3) {
        static const uint32_t rates[3] = { 44100, 48000, 32000 };
        unsigned int version, index;
        uint32_t decoded_rate;
        if (size < 4 || p[0] != 0xff || (p[1] & 0xe0) != 0xe0 ||
            ((p[1] >> 1) & 3) != 1 || (p[2] >> 4) == 15 ||
            (p[3] & 3) == 2) return -1; /* Layer III; no reserved bitrate/emphasis */
        version = (p[1] >> 3) & 3; index = (p[2] >> 2) & 3;
        if (version == 1 || index == 3) return -1;
        decoded_rate = rates[index];
        if (version != 3) decoded_rate >>= version == 2 ? 1 : 2;
        if (decoded_rate != rate) return -1;
        return version == 3 ? 1152 : 576;
    }
#if PPU_ENABLE_OPUS
    if (type == PPU_AUDIO_OPUS) return opus_packet_get_nb_samples(p, (opus_int32)size, 48000);
#endif
    (void)rate;
    if (type != PPU_AUDIO_FLAC || size < 6 || p[0] != 0xff ||
        (p[1] & 0xfe) != 0xf8 || (p[3] & 1)) return -1;
    code = p[2] >> 4;
    if (code == 1) return 192;
    if (code >= 2 && code <= 5) return 576 << (code - 2);
    if (code >= 8) return 256 << (code - 8);
    if (code != 6 && code != 7) return -1;
    /* Skip FLAC's UTF-8 coded frame/sample number before block-size suffix. */
    pos = 4; n = 0;
    if (!(p[pos] & 0x80)) n = 1;
    else {
        unsigned int mask = 0x80;
        while (mask && (p[pos] & mask)) { ++n; mask >>= 1; }
        if (n < 2 || n > 7) return -1;
    }
    if (size - pos < n + (code == 7 ? 2U : 1U)) return -1;
    for (unsigned int i = 1; i < n; ++i)
        if ((p[pos + i] & 0xc0) != 0x80) return -1;
    pos += n;
    return code == 6 ? p[pos] + 1 : (int)be16(p + pos) + 1;
}
