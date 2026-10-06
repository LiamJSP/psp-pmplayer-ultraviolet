#include "audio_stream.h"
#include "audio_software.h"
#include "audio_pcm_vfpu.h"
#include "audiodecoder.h"
#include "cpu_clock.h"
#include "common/mem64.h"
#include <samplerate.h>
#include <pspkernel.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#ifndef PPU_AUDIO_SINC_DEFAULT
#define PPU_AUDIO_SINC_DEFAULT 0
#endif
#define PPU_SRC_INPUT_FRAMES 512U
#define PPU_AUDIO_PACKETS_PER_CALL 32U

struct ppu_audio_stream {
    struct ppu_audio_format format;
    struct ppu_soft_audio *software;
    SRC_STATE *src;
    int native, linear, eof, drained, guard_added, anchored, packet_anchor_valid, tail_trimmed;
    unsigned int skip, pcm_count, pcm_offset, input_count, pending_count;
    uint64_t input_total, output_total, submitted;
    uint64_t raw_frames;
    int64_t anchor_us, packet_anchor_us;
    double src_ratio;
    int16_t pcm[PPU_AUDIO_MAX_DECODE_FRAMES * 2] __attribute__((aligned(16)));
    float input[(PPU_SRC_INPUT_FRAMES + 2) * 2] __attribute__((aligned(16)));
    float output[PPU_AUDIO_BLOCK * 2] __attribute__((aligned(16)));
    int16_t pending[PPU_AUDIO_BLOCK * 2] __attribute__((aligned(16)));
    float last[2];
};

unsigned int ppu_audio_stream_thread_attributes(const struct ppu_audio_stream *s)
{
    /* MP4/MKV call this before creating the sole decode producer, including
     * Audio Only Mode. Selectable tracks have the same decoded rate, so a
     * track replacement cannot newly require VFPU on a 44100-Hz thread.
     * Direct PCM creates no extra VFPU context. No VFPU executes at open. */
    return s && s->src && ppu_audio_pcm_vfpu_enabled() ? PSP_THREAD_ATTR_VFPU : 0U;
}

static int output_timestamp(const struct ppu_audio_stream *s)
{
    int64_t us = s->anchor_us + (int64_t)(s->submitted * 1000000ULL / PPU_AUDIO_RATE);
    int64_t ms = us / 1000;
    return ms > INT_MAX ? INT_MAX : (ms < 0 ? 0 : (int)ms);
}

int ppu_audio_stream_reset(struct ppu_audio_stream *s, int at_start)
{
    if (!s) return 0;
    if (s->native && audio_decoder_open(s->format.type == PPU_AUDIO_MP3 ?
            PSP_CODEC_AUDIO_MP3 : PSP_CODEC_AUDIO_AAC, s->format.rate,
            s->format.max_frames, 0) < 0) return 0;
    if (s->software && !ppu_soft_audio_reset(s->software)) return 0;
    if (s->src && (src_reset(s->src) ||
        src_set_ratio(s->src, s->src_ratio))) return 0;
    s->eof = s->drained = s->guard_added = s->anchored = s->packet_anchor_valid = 0;
    s->tail_trimmed = 0;
    s->pcm_count = s->pcm_offset = s->input_count = s->pending_count = 0;
    s->input_total = s->output_total = s->submitted = s->raw_frames = 0;
    s->anchor_us = s->packet_anchor_us = 0;
    s->last[0] = s->last[1] = 0.0f;
    s->skip = at_start ? (s->format.mp4_timeline ? 0 : s->format.start_skip) :
                         s->format.preroll_frames;
    return 1;
}

struct ppu_audio_stream *ppu_audio_stream_open(const struct ppu_audio_format *f)
{
    struct ppu_audio_stream *s;
    int error = 0, converter;
    if (!f || f->rate < 8000U || f->rate > 48000U ||
        (f->channels != 1U && f->channels != 2U) || !f->max_frames ||
        f->max_frames > PPU_AUDIO_MAX_DECODE_FRAMES) return 0;
    s = malloc_64(sizeof(*s));
    if (!s) return 0;
    memset(s, 0, sizeof(*s));
    s->format = *f;
    s->src_ratio = (double)PPU_AUDIO_RATE / f->rate;
    s->native = f->type == PPU_AUDIO_AAC || f->type == PPU_AUDIO_MP3;
    if (!s->native) {
        s->software = ppu_soft_audio_open(f);
        if (!s->software) goto fail;
    }
    /* No resampler allocation or float conversion on the 44.1-kHz path.
     * Sinc is a documented opt-in until Allegrex timing evidence exists.
     * Battery Saver always selects linear once, at stream open. */
    converter = PPU_AUDIO_SINC_DEFAULT && !cpu_clock_get_extreme_battery_saver() ?
                  SRC_SINC_FASTEST : SRC_LINEAR;
    s->linear = converter == SRC_LINEAR;
    if (f->rate != PPU_AUDIO_RATE) {
        s->src = src_new(converter, 2, &error);
        if (!s->src || error) goto fail;
    }
    if (!ppu_audio_stream_reset(s, 1)) goto fail;
    return s;
fail:
    ppu_audio_stream_close(s);
    return 0;
}

void ppu_audio_stream_close(struct ppu_audio_stream *s)
{
    if (!s) return;
    if (s->src) src_delete(s->src);
    ppu_soft_audio_close(s->software);
    if (s->native) audio_decoder_close();
    free_64(s);
}

/* The packet remains owned until synchronous decode completes. All returned
 * PCM belongs to the stream; no demux pointer crosses a backpressure return. */
static char *decode_packet(struct ppu_audio_stream *s, struct ppa_media_packet *p)
{
    int frames, bytes = 0;
    unsigned int begin, end, skip;
    int64_t packet_us = (int64_t)p->timestamp * 1000;
    uint64_t position = p->audio_sample_position;
    if (!p->data || !p->size || p->size > PPU_AUDIO_MAX_PACKET_BYTES)
        return "audio: invalid compressed packet size";
    if (s->native) {
        /* Reject an invalid Layer III header before entering Sony's codec.
         * Keep the firmware workspace request at its original maximum, but
         * copy only the validated PCM duration (576/1152 for MPEG-2/1 MP3). */
        frames = ppu_audio_packet_frames(s->format.type, p->data, p->size, s->format.rate);
        if (frames <= 0 || (unsigned int)frames > s->format.max_frames)
            return "audio: invalid native frame duration";
        if (audio_decoder_decode(s->pcm, &bytes, p->data, p->size, frames) < 0 ||
            bytes <= 0 || (bytes & 3) || (unsigned int)bytes > sizeof(s->pcm))
            return "audio: native decoder rejected packet";
        if (frames != bytes / 4) return "audio: invalid native frame duration";
    } else {
        frames = ppu_soft_audio_decode(s->software, p->data, p->size,
                                        s->pcm, s->format.max_frames);
        if (frames <= 0) return "audio: software decoder rejected packet";
    }
    if (s->format.mp4_timeline && !(p->audio_flags & 1U))
        return "audio: missing MP4 source sample position";
    if (p->audio_flags & 1U) {
        if (position > (uint64_t)INT64_MAX / 1000000ULL)
            return "audio: sample timestamp overflow";
        packet_us = (int64_t)(position * 1000000ULL / s->format.rate);
    }
    /* Keep normal millisecond quantization, but never hide a real container
     * discontinuity by joining unrelated PCM. Seeks explicitly reset state. */
    if (s->packet_anchor_valid) {
        int64_t expected = s->packet_anchor_us +
            (int64_t)(s->raw_frames * 1000000ULL / s->format.rate);
        if (packet_us - expected > 50000 || expected - packet_us > 50000)
            return "audio: unsupported timestamp gap or overlap";
    } else {
        if (packet_us == 0 && s->format.type == PPU_AUDIO_OPUS)
            s->skip = s->format.mp4_timeline ? 0 : s->format.start_skip;
        s->packet_anchor_us = packet_us;
        s->packet_anchor_valid = 1;
    }
    s->raw_frames += (unsigned int)frames;
    begin = p->audio_trim_start; end = (unsigned int)frames;
    if (begin > end || p->audio_trim_end > end - begin)
        return "audio: invalid discard padding";
    end -= p->audio_trim_end;
    skip = s->skip < (unsigned int)frames ? s->skip : (unsigned int)frames;
    s->skip -= skip;
    if (begin < skip) begin = skip;
    if (s->format.mp4_timeline) {
        if (!p->audio_duration || p->audio_duration > (unsigned int)frames)
            return "audio: inconsistent MP4 packet duration";
        if (end > p->audio_duration) end = p->audio_duration;
        if (position < s->format.start_skip) {
            uint64_t amount = s->format.start_skip - position;
            if (amount > (unsigned int)frames) amount = (unsigned int)frames;
            if (begin < amount) begin = (unsigned int)amount;
        }
        if (s->format.end_sample) {
            uint64_t amount = position < s->format.end_sample ? s->format.end_sample - position : 0;
            if (end > amount) end = (unsigned int)amount;
        }
    }
    if (begin >= end) { s->pcm_count = s->pcm_offset = 0; return 0; }
    if (s->tail_trimmed || (s->anchored && p->audio_trim_start))
        return "audio: unsupported interior discard padding";
    if (p->audio_trim_end) s->tail_trimmed = 1;
    if (!s->anchored) {
        s->anchor_us = packet_us +
            ((int64_t)begin - s->format.delay_samples) * 1000000LL / s->format.rate;
        s->anchored = 1;
    }
    s->pcm_offset = begin; s->pcm_count = end;
    s->input_total += end - begin;
    return 0;
}

char *ppu_audio_stream_read(struct ppu_audio_stream *s,
                            ppu_audio_read_packet read_packet, void *reader,
                            unsigned int track, int16_t *out, int *timestamp,
                            unsigned int *work_us)
{
    unsigned int started = sceKernelGetSystemTimeLow(), io_us = 0, packets = 0;
    char *error = 0, *blocked = 0;
    if (work_us) *work_us = 0;
    if (!s || !read_packet || !out || !timestamp) return "audio: invalid stream";
    if (s->drained) return PPU_AUDIO_EOF;
    while (s->pending_count < PPU_AUDIO_BLOCK) {
        unsigned int want = s->src ? PPU_SRC_INPUT_FRAMES - s->input_count :
                                   PPU_AUDIO_BLOCK - s->pending_count;
        while (want && !blocked) {
            unsigned int n;
            if (s->pcm_offset == s->pcm_count) {
                struct ppa_media_packet packet;
                unsigned int io_started;
                if (s->eof) break;
                if (packets++ == PPU_AUDIO_PACKETS_PER_CALL) { blocked = PPU_AUDIO_AGAIN; break; }
                ppa_media_packet_reset(&packet);
                io_started = sceKernelGetSystemTimeLow();
                error = read_packet(reader, track, &packet);
                io_us += sceKernelGetSystemTimeLow() - io_started;
                if (error) {
                    ppa_media_packet_release(&packet);
                    if (!strcmp(error, PPU_AUDIO_EOF)) { s->eof = 1; error = 0; }
                    else if (!strcmp(error, PPU_AUDIO_AGAIN)) { blocked = error; error = 0; }
                    else goto done;
                    break;
                }
                error = decode_packet(s, &packet);
                ppa_media_packet_release(&packet);
                if (error) goto done;
                if (s->pcm_offset == s->pcm_count) continue;
            }
            n = s->pcm_count - s->pcm_offset;
            if (n > want) n = want;
            if (s->src) {
                ppu_audio_pcm_to_float(s->pcm + s->pcm_offset * 2,
                    s->input + s->input_count * 2, n * 2);
                s->input_count += n;
                s->last[0] = s->input[(s->input_count - 1) * 2];
                s->last[1] = s->input[(s->input_count - 1) * 2 + 1];
            } else {
                memcpy(s->pending + s->pending_count * 2, s->pcm + s->pcm_offset * 2, n * 4);
                s->pending_count += n; s->output_total += n;
            }
            s->pcm_offset += n; want -= n;
        }
        if (s->src) {
            SRC_DATA data;
            uint64_t available = s->input_total * PPU_AUDIO_RATE / s->format.rate;
            unsigned int capacity = PPU_AUDIO_BLOCK - s->pending_count;
            /* Linear retains one source sample of interpolation history. Two
             * repeated guard samples allow its final fraction to drain; output
             * is still capped to the duration of real, trimmed input only. */
            if (s->eof && s->linear && !s->guard_added &&
                s->input_count <= PPU_SRC_INPUT_FRAMES - 2 && s->input_total) {
                for (int i = 0; i < 2; ++i) {
                    s->input[s->input_count * 2] = s->last[0];
                    s->input[s->input_count++ * 2 + 1] = s->last[1];
                }
                s->guard_added = 1;
            }
            if (available > s->output_total && available - s->output_total < capacity)
                capacity = (unsigned int)(available - s->output_total);
            else if (available <= s->output_total) capacity = 0;
            memset(&data, 0, sizeof(data));
            data.data_in = s->input; data.data_out = s->output;
            data.input_frames = s->input_count; data.output_frames = capacity;
            data.src_ratio = s->src_ratio;
            data.end_of_input = s->eof;
            if (capacity && src_process(s->src, &data)) { error = "audio: resampler failed"; goto done; }
            if (data.input_frames_used < 0 || (unsigned long)data.input_frames_used > s->input_count ||
                data.output_frames_gen < 0 || (unsigned long)data.output_frames_gen > capacity) {
                error = "audio: invalid resampler counts"; goto done;
            }
            s->input_count -= (unsigned int)data.input_frames_used;
            if (s->input_count && data.input_frames_used)
                memmove(s->input, s->input + data.input_frames_used * 2, s->input_count * 2 * sizeof(float));
            if (data.output_frames_gen) {
                ppu_audio_float_to_pcm(s->output, s->pending + s->pending_count * 2, data.output_frames_gen * 2);
                s->pending_count += (unsigned int)data.output_frames_gen;
                s->output_total += (unsigned int)data.output_frames_gen;
            }
            if (s->eof && (!capacity || (!data.input_frames_used && !data.output_frames_gen))) {
                if (s->output_total < available) { error = "audio: incomplete resampler drain"; goto done; }
                s->drained = 1;
            } else if (!data.input_frames_used && !data.output_frames_gen &&
                       s->input_count >= PPU_SRC_INPUT_FRAMES) {
                error = "audio: resampler made no progress"; goto done;
            }
        } else if (s->eof) s->drained = 1;
        if (s->pending_count == PPU_AUDIO_BLOCK || s->drained) break;
        if (blocked) { error = blocked; goto done; }
    }
    if (!s->pending_count) { error = PPU_AUDIO_EOF; goto done; }
    *timestamp = output_timestamp(s);
    memcpy(out, s->pending, s->pending_count * 4);
    if (s->pending_count < PPU_AUDIO_BLOCK)
        memset(out + s->pending_count * 2, 0, (PPU_AUDIO_BLOCK - s->pending_count) * 4);
    s->pending_count = 0;
    s->submitted += PPU_AUDIO_BLOCK;
done:
    if (work_us) *work_us = sceKernelGetSystemTimeLow() - started - io_us;
    return error;
}

void ppu_audio_stream_silence(struct ppu_audio_stream *s, int16_t *out, int *timestamp)
{
    memset(out, 0, PPU_AUDIO_BLOCK * 4);
    *timestamp = output_timestamp(s);
    s->submitted += PPU_AUDIO_BLOCK;
}
