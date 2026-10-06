#ifndef __PPA_VFS_H__
#define __PPA_VFS_H__

#include <psptypes.h>
#include <stdint.h>
#include "directory.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ppa_vfs_file ppa_vfs_file;

enum PpaVfsCapability {
	PPA_VFS_CAP_POSITIONED_READ  = 1u << 0,
	PPA_VFS_CAP_ASYNC_READ       = 1u << 1,
	PPA_VFS_CAP_EXPENSIVE_SEEK   = 1u << 2,
	PPA_VFS_CAP_RAW_CACHE_BACKED = 1u << 3,
	PPA_VFS_CAP_REMOVABLE_MEDIA  = 1u << 4
};

int ppa_vfs_init(void);
void ppa_vfs_shutdown(void);

int ppa_vfs_is_ntfs_path(const char *path);
int ppa_vfs_ntfs_available(void);
int ppa_vfs_path_is_supported(const char *path);

int ppa_vfs_open_directory(const char *path,
                           int show_hidden,
                           int show_unknown,
                           file_type_ext_struct *filter,
                           directory_item_struct **items);

ppa_vfs_file *ppa_vfs_open(const char *path);
void ppa_vfs_close(ppa_vfs_file *file);

uint32_t ppa_vfs_capabilities(ppa_vfs_file *file);
uint32_t ppa_vfs_recommended_burst(ppa_vfs_file *file);

int64_t ppa_vfs_size64(ppa_vfs_file *file);
int64_t ppa_vfs_seek64(ppa_vfs_file *file, int64_t position);
int64_t ppa_vfs_tell64(ppa_vfs_file *file);
int ppa_vfs_read_at64(ppa_vfs_file *file, uint64_t position,
                      void *buffer, unsigned int size);
/* Positioned streaming read for cache/window owners. It preserves the public
 * logical cursor but intentionally leaves a direct descriptor at the end of
 * the read so adjacent pump bursts do not issue redundant seeks. */
int ppa_vfs_stream_read_at64(ppa_vfs_file *file, uint64_t position,
                             void *buffer, unsigned int size);

/* I/O-pump entry point. Exactly one pump worker calls this for a given file.
 * Native descriptor async is attempted only when advertised and requested.
 * After the first native-async failure, the file permanently uses synchronous
 * backend calls on the pump worker for the remainder of that open lifetime. */
int ppa_vfs_worker_read_at64(ppa_vfs_file *file,
                             uint64_t position,
                             void *buffer,
                             unsigned int size,
                             int prefer_native_async,
                             int *out_used_native_async,
                             int *out_async_fallback);

/* Source-compatible 32-bit wrappers retained for non-playback callers. */
int ppa_vfs_size(ppa_vfs_file *file);
int ppa_vfs_seek(ppa_vfs_file *file, int position);
int ppa_vfs_tell(ppa_vfs_file *file);
int ppa_vfs_read(ppa_vfs_file *file, void *buffer, unsigned int size);
int ppa_vfs_read_at(ppa_vfs_file *file, int position,
                    void *buffer, unsigned int size);
int ppa_vfs_stream_read_at(ppa_vfs_file *file, int position,
                           void *buffer, unsigned int size);

#ifdef __cplusplus
}
#endif

#endif
