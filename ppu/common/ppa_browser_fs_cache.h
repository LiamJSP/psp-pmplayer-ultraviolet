#ifndef PPA_BROWSER_FS_CACHE_H
#define PPA_BROWSER_FS_CACHE_H

#include "directory.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Menu-only filesystem name cache.
 *
 * The cache deliberately stores only directory hierarchy, display names,
 * short names and file type. File size/date fields and container metadata are
 * never persisted. Returned directory_item_struct arrays are owned by the
 * caller and must be freed with free().
 */
int  ppa_browser_fs_cache_init(int show_hidden,
                               int show_unknown,
                               file_type_ext_struct *filter);
void ppa_browser_fs_cache_shutdown(void);

/* Legacy optional bounded spider API. Automatic startup prefetch is disabled;
 * ordinary browsing learns one directory at a time instead. */
int  ppa_browser_fs_cache_start_build(void);
void ppa_browser_fs_cache_cancel_build(int wait_for_idle);

/* Interactive directory I/O preempts the startup spider's current filesystem
 * turn without discarding the partial cache or abandoning the original
 * 15-second deadline. Generation tokens prevent an older canceled request from
 * clearing a newer request's preemption. */
void ppa_browser_fs_cache_request_interactive_io(unsigned int generation);
void ppa_browser_fs_cache_finish_interactive_io(unsigned int generation);
void ppa_browser_fs_cache_clear_interactive_io(void);

/* Nonblocking UI lookup: a busy cache is a miss, so navigation can ask the
 * existing directory worker instead of waiting behind cache serialization. */
int ppa_browser_fs_cache_get(const char *path,
                             const char *short_path,
                             directory_item_struct **items,
                             int *item_count,
                             int *complete);

int ppa_browser_fs_cache_put(const char *path,
                             const char *short_path,
                             const directory_item_struct *items,
                             int item_count,
                             int complete);

/* Explicit filesystem mutations (for example the browser's delete command)
 * invalidate one cached directory and the persisted snapshot. Ordinary
 * browsing never calls this. */
void ppa_browser_fs_cache_invalidate(const char *path,
                                     const char *short_path);

/* Playback checkpoints changed directory names to PSP/SYSTEM and reclaims
 * RAM. Unchanged snapshots are never rewritten. */
int  ppa_browser_fs_cache_prepare_playback(void);
int  ppa_browser_fs_cache_restore_after_playback(void);

/* Suspend never waits from the power callback. The UI thread calls these only
 * after it has observed a queued power transition. */
void ppa_browser_fs_cache_suspend(void);
void ppa_browser_fs_cache_resume(void);

unsigned int ppa_browser_fs_cache_ram_bytes(void);
int          ppa_browser_fs_cache_available(void);

#ifdef __cplusplus
}
#endif

#endif
