#ifndef PPA_PLAYBACK_ERROR_H
#define PPA_PLAYBACK_ERROR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct h264x_probe;
enum ppa_media_rejection {
    PPA_REJECT_NONE, PPA_REJECT_VIDEO_CODEC, PPA_REJECT_VIDEO_SIZE,
    PPA_REJECT_MKV_BASELINE_SIZE, PPA_REJECT_VIDEO_HEADERS,
    PPA_REJECT_VIDEO_DIMENSIONS, PPA_REJECT_NO_VIDEO,
    PPA_REJECT_NO_AUDIO, PPA_REJECT_AUDIO_CODEC, PPA_REJECT_AUDIO_CHANNELS,
    PPA_REJECT_AUDIO_RATE, PPA_REJECT_AUDIO_CONFIG, PPA_REJECT_FRAME_RATE
};
#define PPA_ERROR_COLUMNS 36U
#define PPA_ERROR_LINES 9U
#define PPA_ERROR_PAGE_BYTES (PPA_ERROR_LINES * (PPA_ERROR_COLUMNS + 1U) + 1U)
struct ppa_error_report {
    char title[40];
    char summary[512];
    char details[1536];
};
/* One playback at a time. Reset on the controller before opening a movie.
 * Open/close owners copy metadata; the decoder records failures. The UI reads
 * only after all playback owners join. No borrowed container pointers, heap,
 * filesystem operations, or additional per-frame parsing. Independent of Debug Mode. */
void ppa_error_reset(void);
void ppa_error_video_track(const char *codec, uint32_t width, uint32_t height);
void ppa_error_audio_track(const char *codec, uint32_t rate, uint32_t channels);
void ppa_error_reject(enum ppa_media_rejection reason);
void ppa_error_frame_rate(uint32_t rate, uint32_t scale);
void ppa_error_h264(const struct h264x_probe *probe);
/* reason must be the same immutable string returned for this failure. */
void ppa_error_native(const char *reason, int code);
/* Called only when the existing Battery Saver check actually rejects. */
void ppa_error_battery_detail(const char *detail);
void ppa_error_battery_headers(const struct h264x_probe *probe, unsigned int profile);
void ppa_error_make_report(const char *result, const char *path,
                           struct ppa_error_report *report);
/* Word-wrap/split long tokens without dropping text. Returns total pages.
 * page_out may be NULL for counting; otherwise provide PPA_ERROR_PAGE_BYTES. */
unsigned int ppa_error_page(const char *text, unsigned int page, char *page_out);
const char *ppa_error_codec(uint32_t fourcc, char fallback[5]);
const char *ppa_error_mkv_codec(const char *codec_id);
void ppa_error_unplayable_audio(uint32_t type, const char *codec,
                                uint32_t rate, uint32_t channels);
#ifdef __cplusplus
}
#endif
#endif
