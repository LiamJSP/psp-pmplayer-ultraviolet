#include "ppa_playback_error.h"
#include "../mod/h264x_probe.h"
#include "../media/VideoPipeline.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static struct {
    enum ppa_media_rejection rejection;
    char video_codec[64], audio_codec[64], battery_detail[160];
    uint32_t width, height, rate, scale, audio_rate, channels;
    struct h264x_sps_info sps;
    struct h264x_pps_info pps;
    unsigned int b_pictures, reference_b_pictures, inspected_aus;
    int inspected, native_code;
    const char *native_reason;
} context;

static void copy_text(char *dst, unsigned int size, const char *src)
{
    unsigned int used = 0;
    const unsigned char *p = (const unsigned char *)(src ? src : "Unknown");
    if (!size) return;
    while (*p && used + 1U < size) {
        unsigned char c = *p++;
        /* The emergency bitmap font is ASCII. Preserve one placeholder per
         * non-ASCII character instead of emitting control bytes or bad glyphs. */
        if (c >= 128U) {
            while ((*p & 0xc0U) == 0x80U) ++p;
            c = '?';
        }
        dst[used++] = c >= 32U && c < 127U ? (char)c : ' ';
    }
    dst[used] = 0;
}
static void append(char *dst, unsigned int size, const char *format, ...)
{
    unsigned int used = (unsigned int)strlen(dst);
    va_list ap;
    if (used >= size) return;
    va_start(ap, format);
    vsnprintf(dst + used, size - used, format, ap);
    va_end(ap);
}
void ppa_error_reset(void) { memset(&context, 0, sizeof(context)); }
void ppa_error_video_track(const char *codec, uint32_t width, uint32_t height)
{
    copy_text(context.video_codec, sizeof(context.video_codec), codec);
    context.width = width; context.height = height;
}
void ppa_error_audio_track(const char *codec, uint32_t rate, uint32_t channels)
{
    copy_text(context.audio_codec, sizeof(context.audio_codec), codec);
    context.audio_rate = rate; context.channels = channels;
}
void ppa_error_reject(enum ppa_media_rejection reason) { context.rejection = reason; }
void ppa_error_frame_rate(uint32_t rate, uint32_t scale)
{ context.rate = rate; context.scale = scale; }
void ppa_error_h264(const struct h264x_probe *p)
{
    if (!p) return;
    /* A second defensive close of an already-reset decoder must not erase
     * metadata saved by its first close. Failed parses never become facts. */
    if (p->sps.valid) context.sps = p->sps;
    if (p->pps.valid) context.pps = p->pps;
    if (p->enabled && p->au_count) {
        context.inspected = 1;
        context.inspected_aus = p->au_count;
        context.b_pictures = p->b_slice_count;
        context.reference_b_pictures = p->reference_b_count;
    }
}
void ppa_error_native(const char *reason, int code)
{ context.native_reason = reason; context.native_code = code; }
void ppa_error_battery_detail(const char *detail)
{
    /* Preserve the first failed check, including when resetting the reader
     * after a rejected probe also fails. */
    if (!context.battery_detail[0])
        copy_text(context.battery_detail, sizeof(context.battery_detail), detail);
}
void ppa_error_battery_headers(const struct h264x_probe *p, unsigned int profile)
{
    char detail[160];
    if (!p || !p->sps.valid || !p->pps.valid || !p->avcc_valid)
        ppa_error_battery_detail("Battery Saver could not verify the H.264 setup headers.");
    else if (profile != 66U && profile != 77U)
        ppa_error_battery_detail("Battery Saver requires H.264 Baseline or Main profile. This file uses a different profile.");
    else if (p->sps.level_idc > 30U)
        ppa_error_battery_detail("The file declares an H.264 level above 3.0, the Battery Saver limit.");
    else if (p->sps.chroma_format_idc > 1U || p->sps.bit_depth_luma_minus8 || p->sps.bit_depth_chroma_minus8)
        ppa_error_battery_detail("Battery Saver requires 8-bit video with 4:2:0 or monochrome sampling.");
    else if (!p->sps.frame_mbs_only_flag || p->sps.mb_adaptive_frame_field_flag)
        ppa_error_battery_detail("Battery Saver requires progressive video. This file permits interlaced H.264 pictures.");
    else if (p->sps.num_ref_frames > (p->pps.entropy_coding_mode_flag ? 2U : 3U)) {
        snprintf(detail, sizeof(detail), "This file declares %u reference frames. Battery Saver allows %u with CABAC %s.",
            p->sps.num_ref_frames, p->pps.entropy_coding_mode_flag ? 2U : 3U,
            p->pps.entropy_coding_mode_flag ? "on" : "off");
        ppa_error_battery_detail(detail);
    } else if (p->pps.num_slice_groups_minus1)
        ppa_error_battery_detail("The file uses multiple H.264 slice groups, which Battery Saver does not allow.");
    else if (p->pps.weighted_pred_flag || p->pps.weighted_bipred_idc)
        ppa_error_battery_detail("The file enables weighted prediction, which Battery Saver does not allow.");
    else if (p->pps.transform_8x8_mode_flag)
        ppa_error_battery_detail("The file enables the H.264 8x8 transform, which Battery Saver does not allow.");
    else if (p->pps.num_ref_idx_l1_default_active_minus1)
        ppa_error_battery_detail("The H.264 default List 1 reference count exceeds the Battery Saver limit of one.");
}

const char *ppa_error_codec(uint32_t code, char fallback[5])
{
    unsigned int i;
    switch (code) {
    case 0x61766331U: return "H.264/AVC";
    case 0x68766331U: case 0x68657631U: return "H.265/HEVC";
    case 0x61763031U: return "AV1";
    case 0x76703039U: return "VP9";
    case 0x6d703476U: return "MPEG-4 Part 2";
    case 0x6d703461U: return "AAC";
    case 0x6d703320U: return "MP3";
    case 0x664c6143U: return "FLAC";
    case 0x4f707573U: return "Opus";
    case 0x61632d33U: return "AC-3";
    case 0x65632d33U: return "E-AC-3";
    case 0: return "Unknown";
    default:
        for (i = 0; i < 4U; ++i) {
            unsigned int c = (code >> ((3U-i)*8U)) & 255U;
            fallback[i] = c >= 32U && c < 127U ? (char)c : '?';
        }
        fallback[4] = 0;
        return fallback;
    }
}
static const char *profile_name(unsigned int profile)
{
    switch (profile) {
    case 66: return "Baseline";
    case 77: return "Main";
    case 88: return "Extended";
    case 100: return "High";
    case 110: return "High 10";
    case 122: return "High 4:2:2";
    case 244: return "High 4:4:4";
    default: return "Other";
    }
}
const char *ppa_error_mkv_codec(const char *id)
{
    if (!id || !*id) return "Unknown";
    if (!strcmp(id, "V_MPEG4/ISO/AVC")) return "H.264/AVC";
    if (!strcmp(id, "V_MPEGH/ISO/HEVC")) return "H.265/HEVC";
    if (!strcmp(id, "V_AV1")) return "AV1";
    if (!strcmp(id, "V_VP9")) return "VP9";
    if (!strcmp(id, "V_VP8")) return "VP8";
    if (!strncmp(id, "A_AAC", 5)) return "AAC";
    if (!strcmp(id, "A_MPEG/L3")) return "MP3";
    if (!strcmp(id, "A_FLAC")) return "FLAC";
    if (!strcmp(id, "A_OPUS")) return "Opus";
    if (!strcmp(id, "A_AC3")) return "AC-3";
    if (!strcmp(id, "A_EAC3")) return "E-AC-3";
    if (!strcmp(id, "A_DTS")) return "DTS";
    return id;
}
void ppa_error_unplayable_audio(uint32_t type, const char *codec,
                                uint32_t rate, uint32_t channels)
{
    ppa_error_audio_track(codec, rate, channels);
    /* Called only after the real format validator rejects every audio track.
     * Explain definite basic limits; other header/timing failures stay generic.
     * This is descriptive and cannot admit/reject a track itself. */
    if (type != 0x6d703461U && type != 0x6d703320U &&
        type != 0x664c6143U && type != 0x4f707573U)
        context.rejection = PPA_REJECT_AUDIO_CODEC;
    else if (channels != 1U && channels != 2U)
        context.rejection = PPA_REJECT_AUDIO_CHANNELS;
    else if (((type == 0x6d703461U || type == 0x6d703320U) &&
             rate != 22050U && rate != 24000U && rate != 32000U &&
             rate != 44100U && rate != 48000U) ||
             (type == 0x664c6143U && (rate < 8000U || rate > 48000U)))
        context.rejection = PPA_REJECT_AUDIO_RATE;
    else context.rejection = PPA_REJECT_AUDIO_CONFIG;
}
static const char *chroma_name(unsigned int chroma)
{
    switch (chroma) {
    case 0: return "monochrome";
    case 1: return "4:2:0";
    case 2: return "4:2:2";
    case 3: return "4:4:4";
    default: return "unknown chroma";
    }
}
static void explain(struct ppa_error_report *r, const char *title, const char *body)
{
    copy_text(r->title, sizeof(r->title), title);
    snprintf(r->summary, sizeof(r->summary), "%s", body);
}
static int has(const char *text, const char *part) { return strstr(text, part) != 0; }
static int video_failure(const char *s)
{
    return has(s, "sceMpegAvcDecode") || has(s, "sceMpegGetAvcNalAu") ||
        has(s, "parameter-set rewrite") || has(s, "explicit weights") ||
        has(s, "compatibility ladder") || has(s, "hardware decoder");
}

void ppa_error_make_report(const char *result, const char *path, struct ppa_error_report *r)
{
    const char *s = result ? result : "Unknown playback error";
    const struct h264x_sps_info *sps = &context.sps;
    const struct h264x_pps_info *pps = &context.pps;
    char raw[384], file[160], level[32];
    if (!r) return;
    memset(r, 0, sizeof(*r));
    explain(r, "Playback stopped",
        "The player could not finish playback.\n\nOpen the details page for the reported error. Enable Debug Mode before retrying to collect a log.");
    switch (context.rejection) {
    case PPA_REJECT_VIDEO_CODEC:
        explain(r, "Video codec unsupported", "");
        snprintf(r->summary, sizeof(r->summary),
            "No playable video track was found.\nRejected codec: %.64s.\n\nConvert the video to H.264/AVC in MP4 or MKV.", context.video_codec);
        break;
    case PPA_REJECT_VIDEO_SIZE: case PPA_REJECT_MKV_BASELINE_SIZE:
        explain(r, "Video size unsupported", "");
        snprintf(r->summary, sizeof(r->summary),
            "Rejected video: %ux%u.\n%s\n\nResize when converting. 480x272 is a suitable PSP screen size.",
            context.width, context.height, context.rejection == PPA_REJECT_MKV_BASELINE_SIZE ?
            "This MKV Baseline path is limited to 480x272." : "Playback limit: 720x480.");
        break;
    case PPA_REJECT_VIDEO_HEADERS:
        explain(r, "Video headers unusable",
            "The H.264 setup headers are missing or invalid.\n\nTry remuxing the original file. If that fails, encode it again."); break;
    case PPA_REJECT_VIDEO_DIMENSIONS:
        explain(r, "Invalid video dimensions",
            "The container reports a zero video width or height.\n\nRemux or re-encode the original file."); break;
    case PPA_REJECT_NO_VIDEO:
        explain(r, "No video track",
            "No video track was found in this file.\n\nCheck that you selected an MP4 or MKV movie with a video track."); break;
    case PPA_REJECT_NO_AUDIO:
        explain(r, "No audio track",
            "No audio track was found. This player currently requires audio even for a silent movie.\n\nAdd an AAC audio track when converting."); break;
    case PPA_REJECT_AUDIO_CODEC:
        explain(r, "Audio codec unsupported", "");
        snprintf(r->summary, sizeof(r->summary),
            "No playable audio track was found.\nRejected codec: %.64s.\n\nConvert the audio to AAC-LC stereo at 44100 or 48000 Hz.", context.audio_codec); break;
    case PPA_REJECT_AUDIO_CHANNELS:
        explain(r, "Too many audio channels", "");
        snprintf(r->summary, sizeof(r->summary),
            "Rejected audio: %u channels.\nThe player accepts mono or stereo.\n\nDownmix to 1 or 2 channels when converting.", context.channels);
        if (context.channels == 0) copy_text(r->title, sizeof(r->title), "Invalid audio channels");
        break;
    case PPA_REJECT_AUDIO_RATE:
        explain(r, "Audio rate unsupported", "");
        snprintf(r->summary, sizeof(r->summary),
            "Rejected audio: %.32s at %u Hz.\n\nConvert the audio to AAC-LC stereo at 44100 or 48000 Hz.", context.audio_codec, context.audio_rate); break;
    case PPA_REJECT_AUDIO_CONFIG:
        explain(r, "Audio setup unsupported",
            "No audio track passed the header and timing checks.\n\nRemux the file, or convert its audio to AAC-LC stereo. See details for the detected format."); break;
    case PPA_REJECT_FRAME_RATE:
        explain(r, "Frame rate too high", "");
        snprintf(r->summary, sizeof(r->summary),
            "Reported frame rate: %llu.%02llu fps.\nThe experimental playback range is up to 60 fps.\n\nConvert to 30 fps for a lighter workload.",
            context.scale ? (unsigned long long)(context.rate / context.scale) : 0ULL,
            context.scale ? (unsigned long long)(((uint64_t)context.rate * 100U / context.scale) % 100U) : 0ULL); break;
    default: break;
    }
    if (context.rejection == PPA_REJECT_NONE) {
        if (!strcmp(s, PPA_BATTERY_SAVER_REJECTED)) {
            explain(r, "Battery Saver limit", "");
            snprintf(r->summary, sizeof(r->summary), "%s\n\nTurn off Battery Saver Mode in Config, then try this file again.",
                context.battery_detail[0] ? context.battery_detail : "This file did not pass the stricter Battery Saver checks.");
        } else if (has(s, "sceKernelCreate") || has(s, "reserve close worker") ||
                   has(s, "could not create seek request lock")) {
            explain(r, "System resource unavailable",
                "A playback thread or synchronization resource could not be created.\n\nRestart PPU. Low memory or unavailable system resources may be the cause.");
        } else if (has(s, "malloc") || has(s, "memalign") || has(s, "allocation") ||
                   has(s, "insufficient memory") || has(s, "can't retain") ||
                   has(s, "can not init")) {
            explain(r, "Not enough memory",
                "A required buffer or system resource could not be allocated.\n\nRestart PPU. Try a smaller file or fewer subtitle tracks, especially on PSP-1000.");
        } else if (has(s, "index exceeds") || has(s, "index too large") || has(s, "index budget")) {
            explain(r, "Movie index too large",
                "The movie's seek/sample index exceeds the player's memory budget.\n\nTry an MKV remux or split the movie into shorter parts.");
        } else if (has(s, "codec_prx") || has(s, "Sony ME boot") || has(s, "sceMpegInit") ||
                   has(s, "sceMpegCreate") || has(s, "sceMpegQueryMemSize") || has(s, "acceleration preparation")) {
            explain(r, "Decoder could not start",
                "The PSP video service could not be initialized.\n\nRestart PPU. Check that EBOOT and all PRX files come from the same build. See details for the error.");
        } else if (has(s, "watchdog") || has(s, "stalled")) {
            explain(r, "Playback stopped responding",
                "The playback pipeline stopped making progress. The error does not identify whether decoding, storage, or another stage stalled.\n\nEnable Debug Mode and retry to collect more information.");
        } else if (has(s, "presentation queue invariant") || has(s, "two-picture recovery") ||
                   has(s, "cached-picture restore")) {
            explain(r, "Frame recovery failed",
                "The player could not safely restore or order decoded pictures. This may be a player or stream compatibility problem.\n\nEnable Debug Mode and report this file's details.");
        } else if (video_failure(s)) {
            if (sps->valid && (sps->bit_depth_luma_minus8 || sps->bit_depth_chroma_minus8)) {
                explain(r, "Video bit depth unsupported", "");
                snprintf(r->summary, sizeof(r->summary),
                    "H.264 depth: %llu-bit luma, %llu-bit chroma.\nThis playback path needs 8-bit video.\n\nConvert to 8-bit H.264, 4:2:0.",
                    (unsigned long long)sps->bit_depth_luma_minus8 + 8ULL,
                    (unsigned long long)sps->bit_depth_chroma_minus8 + 8ULL);
            } else if (sps->valid && sps->chroma_format_idc > 1U) {
                explain(r, "Video color format unsupported", "");
                snprintf(r->summary, sizeof(r->summary),
                    "The file uses H.264 %s color sampling.\n\nConvert to 8-bit H.264 with 4:2:0 color sampling.", chroma_name(sps->chroma_format_idc));
            } else if (sps->valid && !sps->frame_mbs_only_flag) {
                explain(r, "Interlaced video detected",
                    "This file permits interlaced H.264 pictures. The playback path expects progressive video.\n\nDeinterlace when converting.");
            } else if (has(s, "explicit weights")) {
                explain(r, "Weighted prediction failed",
                    "H264X could not safely adapt this stream's weighted prediction.\n\nEncode with weighted prediction disabled, then try again.");
            } else {
                explain(r, "Video decoder stopped",
                    "The PSP decoder could not process this stream. The exact cause is unknown.\n\nTry a simpler 8-bit H.264 encode. Details show the detected encoding settings.");
            }
        } else if (has(s, "can not read") || has(s, "read failed") || has(s, "file seek failed") || has(s, "sample seek failed")) {
            explain(r, "Could not read movie data",
                "Reading or seeking in the file failed. The file may be incomplete, or storage may have returned an error.\n\nCopy the movie again and check the card.");
        } else if (has(s, "can't open file")) {
            explain(r, "Could not open the movie",
                "The file could not be opened or parsed. Its contents, storage access, or available memory may be the cause.\n\nCheck the file on a computer and copy it again.");
        } else if (has(s, "audio:") || has(s, "audio output") || has(s, "sceAudio")) {
            explain(r, "Audio playback failed",
                "The audio decoder, timing check, or output service failed.\n\nTry another audio track, or convert the audio to AAC-LC stereo. See details for the reported reason.");
        } else if (has(s, "avcC") || has(s, "SPS") || has(s, "PPS") || has(s, "parameter set")) {
            explain(r, "Video headers unusable",
                "The H.264 setup headers could not be used.\n\nTry remuxing the original file. If that fails, encode it again.");
        } else if (has(s, "timing") || has(s, "timescale") || has(s, "timebase") || has(s, "timestamp")) {
            explain(r, "Movie timing unsupported",
                "The movie's timestamps or timing tables could not be used.\n\nTry remuxing the original file. If needed, convert to a constant frame rate.");
        } else if (has(s, "cluster") || has(s, "sample index") || has(s, "auxiliary media track")) {
            explain(r, "Container layout unsupported",
                "Required movie data or index information could not be used.\n\nRemux the video and audio into a fresh MP4 or MKV file.");
        } else if (has(s, "display") || has(s, "surface") || has(s, "framebuffer") ||
                   has(s, "sceMpegBaseCscAvc") || has(s, "invalid AVC detail") ||
                   has(s, "invalid decoded frame dimensions") || has(s, "invalid cached-frame")) {
            explain(r, "Video output failed",
                "A decoded frame could not be converted or displayed.\n\nRestart PPU. If it repeats, enable Debug Mode and report the details.");
        } else if (has(s, "sceKernelSignalSema") || has(s, "sceKernelWaitSema") || has(s, "worker start failed")) {
            explain(r, "Playback coordination failed",
                "A playback worker could not start or synchronize. This error alone does not indicate an incompatible encode.\n\nRestart PPU. If it repeats, enable Debug Mode and report the details.");
        } else if (has(s, "unsupported movie container")) {
            explain(r, "Container unsupported", "This player opens MP4 and MKV movies.\n\nUse one of those containers with H.264 video and a supported audio track.");
        }
    }
    copy_text(raw, sizeof(raw), s);
    copy_text(file, sizeof(file), path ? path : "Unknown file");
    append(r->details, sizeof(r->details), "File: %s\n\nReported error:\n%s\n", file, raw);
    if (context.native_reason && !strcmp(s, context.native_reason))
        append(r->details, sizeof(r->details), "Decoder code: 0x%08x\n", (unsigned)context.native_code);
    if (context.video_codec[0]) append(r->details, sizeof(r->details), "\nVideo: %s %ux%u\n", context.video_codec, context.width, context.height);
    if (context.rate && context.scale) append(r->details, sizeof(r->details), "Frame rate: %u/%u fps\n", context.rate, context.scale);
    if (sps->valid) {
        if (sps->level_idc == 11U && (sps->constraint_flags & 0x10U) &&
            (sps->profile_idc == 66U || sps->profile_idc == 77U || sps->profile_idc == 88U))
            snprintf(level, sizeof(level), "1b");
        else if (sps->level_idc == 9U) snprintf(level, sizeof(level), "1b");
        else snprintf(level, sizeof(level), "%u.%u", sps->level_idc / 10U, sps->level_idc % 10U);
        append(r->details, sizeof(r->details), "Profile: %s (%u)\nLevel: %s (idc %u)\nCoded size: %ux%u\n",
            profile_name(sps->profile_idc), sps->profile_idc, level, sps->level_idc, sps->coded_width, sps->coded_height);
        append(r->details, sizeof(r->details), "%llu/%llu-bit %s\n%s\nReference frames: %u\n",
            (unsigned long long)sps->bit_depth_luma_minus8 + 8ULL, (unsigned long long)sps->bit_depth_chroma_minus8 + 8ULL,
            chroma_name(sps->chroma_format_idc), sps->frame_mbs_only_flag ? "Progressive frames" : "Interlaced pictures permitted", sps->num_ref_frames);
    } else append(r->details, sizeof(r->details), "H.264 SPS details unavailable.\n");
    if (pps->valid) append(r->details, sizeof(r->details), "CABAC: %s\nWeighted prediction: %s\n8x8 transform: %s\n",
        pps->entropy_coding_mode_flag ? "on" : "off", (pps->weighted_pred_flag || pps->weighted_bipred_idc) ? "on" : "off",
        pps->transform_8x8_mode_flag ? "on" : "off");
    if (context.inspected) append(r->details, sizeof(r->details),
        "Observed since last seek:\nAccess units: %u\nB pictures: %u\nReference B pictures: %u\nB-pyramid: %s\n",
        context.inspected_aus, context.b_pictures, context.reference_b_pictures,
        context.reference_b_pictures ? "reference B observed" : "not established");
    else append(r->details, sizeof(r->details), "B-frame/pyramid observations unavailable.\n");
    if (context.audio_codec[0]) append(r->details, sizeof(r->details), "\nAudio: %s\n%u channels, %u Hz\n",
        context.audio_codec, context.channels, context.audio_rate);
    append(r->details, sizeof(r->details), "\nEncoding details describe the source; they do not identify every firmware failure. Debug Mode can collect more information on the next attempt.");
}

unsigned int ppa_error_page(const char *text, unsigned int page, char *out)
{
    const char *p = text ? text : "";
    unsigned int line = 0U, used = 0U;
    if (out) out[0] = 0;
    while (*p) {
        unsigned int n = 0U, split = 0U, take, i;
        while (p[n] && p[n] != '\n' && n < PPA_ERROR_COLUMNS) {
            if (p[n] == ' ') split = n;
            ++n;
        }
        take = n;
        if (n == PPA_ERROR_COLUMNS && p[n] && p[n] != '\n' && p[n] != ' ' && split)
            take = split;
        if (out && line / PPA_ERROR_LINES == page) {
            for (i = 0; i < take; ++i) out[used++] = p[i];
            out[used++] = '\n';
            out[used] = 0;
        }
        p += take;
        if (*p == '\n') ++p;
        else while (*p == ' ') ++p;
        ++line;
    }
    return line ? (line + PPA_ERROR_LINES - 1U) / PPA_ERROR_LINES : 1U;
}
