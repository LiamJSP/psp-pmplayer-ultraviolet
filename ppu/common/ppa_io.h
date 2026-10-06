#ifndef __PPA_IO_H__
#define __PPA_IO_H__

#include <psptypes.h>
#include <pspiofilemgr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPA_IO_DEFAULT_RETRIES      4
#define PPA_IO_DEFAULT_CHUNK_SIZE   (128 * 1024)

/*
 * Central retrying I/O helpers for Memory Stick reads.
 *
 * These helpers are intentionally small and PSP-specific:
 * - bounded retries only, never infinite loops;
 * - short sleeps between retries to give the MS driver/adapter time to recover;
 * - exact-read helpers for parsers that cannot safely continue with partial data;
 * - async helpers that fall back to sync reads if async I/O flakes out.
 */

typedef int (*ppa_io_cancel_check_fn)(void *user);

/* A menu background worker may install a lightweight generation check.
 * Playback cancels and joins that worker before opening media, so the hook is
 * never shared with decoder I/O. */
void ppa_io_set_cancel_check(ppa_io_cancel_check_fn check, void *user);
int  ppa_io_cancel_requested(void);

/* Serialize low-priority browser/cache/parser access to filesystem backends
 * that are not safely re-entrant (notably the userspace NTFS volume). Waiting
 * is cancellable and happens only on background threads, never in the UI. */
int  ppa_io_background_init(void);
void ppa_io_background_shutdown(void);
int  ppa_io_background_acquire(ppa_io_cancel_check_fn check, void *user);
void ppa_io_background_release(void);

SceUID ppa_io_open_read_retry(const char *path, int flags, int mode);

int ppa_io_seek32_retry(SceUID fd, int offset, int whence);
SceOff ppa_io_seek_retry(SceUID fd, SceOff offset, int whence);

int ppa_io_read_retry(SceUID fd, void *buffer, unsigned int size);
int ppa_io_read_exact_retry(SceUID fd, void *buffer, unsigned int size);

int ppa_io_read_exact_at32_retry(SceUID fd,
                                 int offset,
                                 void *buffer,
                                 unsigned int size);

int ppa_io_read_exact_at_retry(SceUID fd,
                               SceOff offset,
                               void *buffer,
                               unsigned int size);

int ppa_io_async_read_start_at32_retry(SceUID fd,
                                       int offset,
                                       void *buffer,
                                       unsigned int size);

int ppa_io_async_read_wait_exact_or_sync_at32(SceUID fd,
                                              int offset,
                                              void *buffer,
                                              unsigned int size);

/* Execute one positioned native-async read and wait for its completion.
 * On adapters that reject or short-complete async I/O, the helper re-reads the
 * whole range synchronously and reports that fallback through out_sync_fallback.
 * The 64-bit offset keeps the playback reader above the historical 2 GiB limit. */
int ppa_io_async_read_exact_at_retry(SceUID fd,
                                     SceOff offset,
                                     void *buffer,
                                     unsigned int size,
                                     int *out_sync_fallback);

#ifdef __cplusplus
}
#endif

#endif
