#ifndef PPA_PLAYBACK_UI_H
#define PPA_PLAYBACK_UI_H
#ifdef __cplusplus
class UiI18n;
int ppa_playback_ui_prepare(UiI18n *ui);
extern "C" {
#endif
void ppa_playback_ui_release(void);
const char *ppa_playback_ui_text(const char *key, const char *fallback);
/* Once enabled and allocated, Health stays visible across governor backoff. */
int ppa_playback_ui_has_overlay(void);
/* Called only inside the container producer's GE list. Composes cached text
 * and digit cells without font I/O, shaping, allocation or blocking telemetry.
 * Submits the cached cropped 4444 glyph rectangles on every video frame.
 * No other thread may write the health texture during normal A/V playback. */
void ppa_playback_ui_draw(void);
/* Audio-only owns a static framebuffer, with no video worker or AVC/GE writer. */
void ppa_playback_ui_tick_audio_only(void);
#ifdef __cplusplus
}
#endif
#endif
