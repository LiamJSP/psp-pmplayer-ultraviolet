#ifndef __PPA_NTFS_H__
#define __PPA_NTFS_H__

#include <psptypes.h>
#include <stdint.h>
#include "directory.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PPA_NTFS_MAX_PATH
#define PPA_NTFS_MAX_PATH 512
#endif

typedef struct ppa_ntfs_file ppa_ntfs_file;

int ppa_ntfs_init(void);
void ppa_ntfs_shutdown(void);
int ppa_ntfs_available(void);

int ppa_ntfs_readdir(const char *path,
                     int show_hidden,
                     int show_unknown,
                     file_type_ext_struct *filter,
                     int names_only,
                     directory_item_struct **out_items);

ppa_ntfs_file *ppa_ntfs_open(const char *path);
void ppa_ntfs_close(ppa_ntfs_file *file);
int64_t ppa_ntfs_seek64(ppa_ntfs_file *file, int64_t position);
int64_t ppa_ntfs_tell64(ppa_ntfs_file *file);
int64_t ppa_ntfs_size64(ppa_ntfs_file *file);
int ppa_ntfs_read_at64(ppa_ntfs_file *file, uint64_t position,
                       void *buffer, unsigned int size);

/* Source-compatible 32-bit wrappers. New playback code uses the 64-bit API. */
int ppa_ntfs_seek(ppa_ntfs_file *file, int position);
int ppa_ntfs_tell(ppa_ntfs_file *file);
int ppa_ntfs_size(ppa_ntfs_file *file);
int ppa_ntfs_read(ppa_ntfs_file *file, void *buffer, unsigned int size);
int ppa_ntfs_read_at(ppa_ntfs_file *file, int position, void *buffer, unsigned int size);

int ppa_ntfs_path_is_ntfs(const char *path);
const char *ppa_ntfs_strip_prefix(const char *path);

#ifdef __cplusplus
}
#endif

#endif