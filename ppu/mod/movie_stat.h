/* Persistent playback settings for the supported AVC containers. */
#ifndef PPA_MOVIE_STAT_H
#define PPA_MOVIE_STAT_H

struct mp4_play_struct;
struct mkv_play_struct;

#define MAX_MOVIE_STAT 20

#ifdef __cplusplus
extern "C" {
#endif

void init_movie_stat(const char *path);
/* Register the currently playing movie for callback-safe resume checkpointing.
 * The path/hash are copied; current_timestamp remains valid until the matching
 * movie_stat_active_end call during backend close. */
void movie_stat_active_begin(const char hash[16], const char *movie_file,
                             volatile int *current_timestamp);
void movie_stat_active_end(volatile int *current_timestamp);
/* Used by HOME-exit and suspend callbacks, whose direct kernel transition can
 * bypass the ordinary MP4/MKV close path. Writes only the ms0 timecode file. */
void movie_stat_emergency_save(void);
void mp4_stat_load(struct mp4_play_struct *player);
void mp4_stat_save(struct mp4_play_struct *player);
void mkv_stat_load(struct mkv_play_struct *player);
void mkv_stat_save(struct mkv_play_struct *player);

#ifdef __cplusplus
}
#endif

#endif
