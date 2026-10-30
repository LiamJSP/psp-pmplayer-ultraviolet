#ifndef PPA_DEBUG_H
#define PPA_DEBUG_H
#ifdef __cplusplus
extern "C" {
#endif
/* Nonblocking controller handshake; the optional writer acknowledges after
 * its current bounded batch has finished. Keep queued records across standby. */
int ppa_debug_park(void);
void ppa_debug_unpark(void);
struct h264x_probe;
struct h264x_au_info;
/* Controller-only lifecycle. Disabled mode creates no files or worker. */
void ppa_debug_init(int configured);
int ppa_debug_set_enabled(int enabled); /* re-enabling always resets to three */
int ppa_debug_enabled(void);
unsigned int ppa_debug_remaining(void);
void ppa_debug_shutdown(void);
void ppa_debug_movie_begin(const char *path, const char *container, int audio_only);
void ppa_debug_movie_started(void); /* immediately before starting workers */
void ppa_debug_movie_start_failed(void); /* refund a failed worker launch */
void ppa_debug_movie_end(const char *result); /* after all workers join */
void ppa_debug_tick(int force); /* controller samples at most once per second */
/* Producers only enqueue/copy bounded data; never perform filesystem I/O. */
void ppa_debug_error(const char *where, int code);
void ppa_debug_event(const char *message);
void ppa_debug_video(const struct h264x_probe *p);
void ppa_debug_media_info(unsigned int width, unsigned int height,
    unsigned int rate, unsigned int scale, unsigned int duration_ms,
    unsigned int audio_codec, unsigned int audio_rate, unsigned int audio_channels,
    int battery_saver);
void ppa_debug_note_au(const struct h264x_au_info *au);
void ppa_debug_discontinuity(void);
void ppa_debug_encoder_options(const void *data, unsigned int bytes, unsigned int length_size);
/* C++ configuration adapter; state on ms0 is authoritative across launches. */
void ppa_debug_config_init(const char *config_path);
int ppa_debug_config_set(int enabled);
void ppa_debug_config_sync(void);
#ifdef __cplusplus
}
#endif
#endif
