/* Native tests of real track admission, error snapshots and pagination.
 * Container metadata is supplied by fixtures; no firmware is emulated. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ppu/common/ppa_playback_error.h"
#include "../ppu/mod/h264x_probe.h"
#include "../ppu/media/VideoPipeline.h"
#include "../ppu/mod/mp4_file.h"
#include "../ppu/mod/mkv_file.h"
const char *__asan_default_options(void) { return "detect_leaks=0"; }
static mp4info_t mp4;
static mkvinfo_t mkv;
static unsigned int closes;
mp4info_t *mp4info_open(const char *p) { (void)p; return &mp4; }
mkvinfo_t *mkvinfo_open(const char *p) { (void)p; return &mkv; }
void mp4info_close(mp4info_t *p) { assert(p == &mp4); ++closes; }
void mkvinfo_close(mkvinfo_t *p) { assert(p == &mkv); ++closes; }
void *malloc_64(int n) { assert(n >= 0); return malloc((unsigned int)n); }
void free_64(void *p) { free(p); }

static struct ppa_error_report report;
static void expect(const char *error, const char *title, const char *detail)
{
    ppa_error_make_report(error, "ms0:/VIDEO/movie.mkv", &report);
    assert(!strcmp(report.title, title));
    if (detail) assert(strstr(report.summary, detail) || strstr(report.details, detail));
}
static void test_snapshot(void)
{
    struct h264x_probe probe = {0}, empty = {0};
    ppa_error_reset();
    probe.sps.valid = probe.pps.valid = probe.enabled = probe.avcc_valid = 1;
    probe.sps.profile_idc = 100; probe.sps.level_idc = 31;
    probe.sps.coded_width = 480; probe.sps.coded_height = 272;
    probe.sps.num_ref_frames = 4; probe.sps.chroma_format_idc = 1;
    probe.sps.frame_mbs_only_flag = 1;
    probe.pps.entropy_coding_mode_flag = 1;
    probe.au_count = 24; probe.b_slice_count = 8; probe.reference_b_count = 3;
    ppa_error_h264(&probe);
    ppa_error_h264(&empty); /* defensive second close must retain evidence */
    ppa_error_native("avc_get: sceMpegAvcDecode failed", (int)0x80628002U);
    expect("avc_get: sceMpegAvcDecode failed", "Video decoder stopped", "exact cause is unknown");
    assert(strstr(report.details, "High (100)") && strstr(report.details, "3.1 (idc 31)"));
    assert(strstr(report.details, "Reference frames: 4") && strstr(report.details, "CABAC: on"));
    assert(strstr(report.details, "B pictures: 8") && strstr(report.details, "reference B observed"));
    assert(strstr(report.details, "0x80628002"));
    probe.sps.bit_depth_luma_minus8 = probe.sps.bit_depth_chroma_minus8 = 2;
    ppa_error_h264(&probe);
    expect("avc_get: sceMpegAvcDecode failed", "Video bit depth unsupported", "10-bit luma");
    expect("mp4_read: can not read sample", "Could not read movie data", "storage");
    assert(!strstr(report.details, "Decoder code:")); /* recovered native error is not this failure */
    expect("mkv playback watchdog: pipeline made no progress", "Playback stopped responding", "storage");
    probe.sps.bit_depth_luma_minus8 = probe.sps.bit_depth_chroma_minus8 = 0;
    probe.sps.chroma_format_idc = 2; ppa_error_h264(&probe);
    expect("avc_get: sceMpegAvcDecode failed", "Video color format unsupported", "4:2:2");
    probe.sps.chroma_format_idc = 1; probe.sps.frame_mbs_only_flag = 0;
    ppa_error_h264(&probe);
    expect("avc_get: sceMpegAvcDecode failed", "Interlaced video detected", "Deinterlace");
    ppa_error_reset();
    expect("new unknown failure", "Playback stopped", "new unknown failure");
    assert(!strstr(report.details, "High (100)") && !strstr(report.details, "Decoder code:"));
    assert(strstr(report.details, "details unavailable"));
    probe.sps.profile_idc = 66; probe.sps.level_idc = 11; probe.sps.constraint_flags = 0x10;
    ppa_error_h264(&probe);
    expect("new unknown failure", "Playback stopped", "Level: 1b (idc 11)");
    ppa_error_reset();
    ppa_error_frame_rate(120000, 1001); ppa_error_reject(PPA_REJECT_FRAME_RATE);
    expect("frame rate rejected", "Frame rate too high", "119.88 fps");
    ppa_error_reset(); probe.sps.frame_mbs_only_flag = 1;
    probe.sps.profile_idc = 77; probe.sps.level_idc = 30;
    ppa_error_battery_headers(&probe, 77);
    ppa_error_battery_detail("later seek failure must not replace first cause");
    expect(PPA_BATTERY_SAVER_REJECTED, "Battery Saver limit", "4 reference frames");
    assert(strstr(report.summary, "allows 2 with CABAC on"));
    assert(!strstr(report.summary, "later seek failure"));
    ppa_error_reset();
    ppa_error_unplayable_audio(0x6d703461U, "AAC", 96000, 2);
    expect("unsupported audio", "Audio rate unsupported", "96000 Hz");
    ppa_error_reset();
    ppa_error_unplayable_audio(0x6d703461U, "AAC", 48000, 6);
    expect("unsupported audio", "Too many audio channels", "6 channels");
}

static void test_containers(void)
{
    struct mp4_file_struct mp4file;
    struct mkv_file_struct mkvfile;
    mp4info_track_t v = {0}, a = {0}, bad = {0};
    mkvinfo_track_t mv = {0}, ma = {0}, mbad = {0};
    unsigned char avcc[] = {1,77,0,30,255,225,0,1,0x67,1,0,1,0x68};
    char *error;
    memset(&mp4, 0, sizeof(mp4)); memset(&mkv, 0, sizeof(mkv));
    v.type = MP4_TRACK_VIDEO; v.width = 1920; v.height = 1080;
    v.video_type = 0x61766331U;
    mp4.tracks[0] = &v; mp4.total_tracks = 1;
    ppa_error_reset(); error = mp4_file_open(&mp4file, "fixture"); assert(error && closes == 1);
    expect(error, "Video size unsupported", "1920x1080");
    v.width = 480; v.height = 272; v.video_type = 0; v.sample_entry_type = 0x68766331U;
    ppa_error_reset(); error = mp4_file_open(&mp4file, "fixture"); assert(error);
    expect(error, "Video codec unsupported", "H.265/HEVC");
    v.video_type = 0x61766331U;
    ppa_error_reset(); error = mp4_file_open(&mp4file, "fixture"); assert(error);
    expect(error, "Video headers unusable", "missing or invalid");
    v.avc_sps = avcc + 8; v.avc_sps_size = 1; v.avc_pps = avcc + 12; v.avc_pps_size = 1;
    v.width = 720; v.height = 480; v.avc_profile = 66;
    ppa_error_reset(); error = mp4_file_open(&mp4file, "fixture"); assert(error);
    expect(error, "No audio track", "720x480"); /* large MP4 Baseline remains admitted */
    v.width = 480; v.height = 272;
    bad = v; bad.width = 1920; bad.height = 1080;
    a.type = MP4_TRACK_AUDIO; a.audio_type = 0x6d703461U; a.channels = 6; a.samplerate = 48000;
    mp4.total_tracks = 3; mp4.tracks[0] = &bad; mp4.tracks[1] = &v; mp4.tracks[2] = &a;
    ppa_error_reset(); error = mp4_file_open(&mp4file, "fixture"); assert(error);
    expect(error, "Too many audio channels", "6 channels");
    assert(strstr(report.details, "480x272") && !strstr(report.details, "1920x1080"));

    mkv.has_first_cluster_pos = 1; mkv.total_tracks = 1; mkv.tracks[0] = &mv;
    mv.type = MATROSKA_TRACK_VIDEO; mv.width = 640; mv.height = 360;
    strcpy(mv.codec_id, "V_MPEGH/ISO/HEVC");
    ppa_error_reset(); error = mkv_file_open(&mkvfile, "fixture"); assert(error);
    expect(error, "Video codec unsupported", "H.265/HEVC");
    mv.video_type = 0x61766331U; strcpy(mv.codec_id, "V_MPEG4/ISO/AVC");
    ppa_error_reset(); error = mkv_file_open(&mkvfile, "fixture"); assert(error);
    expect(error, "Video headers unusable", "missing or invalid");
    mv.private_data = avcc; mv.private_size = sizeof(avcc); avcc[1] = 66;
    ppa_error_reset(); error = mkv_file_open(&mkvfile, "fixture"); assert(error);
    expect(error, "Video size unsupported", "MKV Baseline path");
    avcc[1] = 77; mbad = mv; mbad.width = 1920; mbad.height = 1080;
    ma.type = MATROSKA_TRACK_AUDIO; strcpy(ma.codec_id, "A_DTS");
    ma.samplerate = 48000; ma.channels = 6;
    mkv.total_tracks = 3; mkv.tracks[0] = &mbad; mkv.tracks[1] = &mv; mkv.tracks[2] = &ma;
    ppa_error_reset(); error = mkv_file_open(&mkvfile, "fixture"); assert(error);
    expect(error, "Audio codec unsupported", "DTS");
    ma.audio_type = 0x6d703461U; ma.channels = 2; strcpy(ma.codec_id, "A_AAC");
    mv.time_scale = 1000000000; mv.duration = 33333333;
    ppa_error_reset(); error = mkv_file_open(&mkvfile, "fixture"); assert(!error);
    mkv_file_close(&mkvfile);
    expect("later unclassified failure", "Playback stopped", "640x360");
    assert(!strstr(report.details, "1920x1080") && !strstr(report.details, "DTS"));
}

static void check_pages(const char *text)
{
    struct { unsigned char before; char page[PPA_ERROR_PAGE_BYTES]; unsigned char after; } out;
    unsigned int count = ppa_error_page(text, 0, NULL), i;
    const char *input = text;
    for (i = 0; i < count; ++i) {
        unsigned int col = 0, lines = 0;
        const char *p;
        memset(&out, 0xa5, sizeof(out));
        assert(ppa_error_page(text, i, out.page) == count);
        assert(out.before == 0xa5 && out.after == 0xa5);
        assert(strlen(out.page) < PPA_ERROR_PAGE_BYTES);
        for (p = out.page; *p; ++p) {
            if (*p == '\n') { assert(col <= PPA_ERROR_COLUMNS); ++lines; col = 0; }
            else ++col;
            if (*p != ' ' && *p != '\n') {
                while (*input == ' ' || *input == '\n') ++input;
                assert(*input++ == *p); /* no lost, duplicated or reordered text */
            }
        }
        assert(lines <= PPA_ERROR_LINES);
    }
    while (*input == ' ' || *input == '\n') ++input;
    assert(!*input);
    assert(ppa_error_page(text, count, out.page) == count && !out.page[0]);
}
int main(void)
{
    unsigned int i, length, random = 7;
    char data[4097];
    test_snapshot(); test_containers();
    check_pages(""); check_pages("\n\n\n"); check_pages("small error");
    memset(data, 'a', sizeof(data)-1); data[sizeof(data)-1] = 0; check_pages(data);
    for (length = 1; length <= 4096; length += 13) {
        for (i = 0; i < length; ++i) {
            random = random * 1664525U + 1013904223U;
            data[i] = random % 7U == 0 ? '\n' : random % 3U == 0 ? ' ' : 'a' + random % 26U;
        }
        data[length] = 0; check_pages(data);
    }
    puts("PASS: actual MP4/MKV rejection paths, multi-track selection, teardown snapshots, native-code matching, encoding explanations and bounded pagination");
    return 0;
}
