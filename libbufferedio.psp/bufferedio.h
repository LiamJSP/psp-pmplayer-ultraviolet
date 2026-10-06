#ifndef __LIB_BUFFERED_IO_H__
#define __LIB_BUFFERED_IO_H__

#include <pspiofilemgr.h>
#include <stdint.h>

#define CACHE_BUFFER_SIZE 4096

/*
 * Buffered metadata reader used by libmkvinfo/libmp4info.
 *
 * mode:
 *   0 = normal PSP firmware SceUID path
 *   1 = PPA VFS path, currently used for synthetic ms1:/ NTFS
 *
 * Positions/cache edges are 64-bit; individual reads remain bounded 32-bit
 * chunks. The legacy ABI rejects oversized files rather than truncating them.
 * Matroska metadata uses the explicit 64-bit entry points; MP4 atom migration
 * is a separate phase, so its legacy callers still reject files above 2 GiB.
 */
typedef struct {
	int mode;

	SceUID handle;
	void *vfs_handle;

	int64_t length;
	int64_t cache_first_position;
	int64_t cache_last_position;
	int64_t current_position;
	uint8_t cache_buffer[CACHE_BUFFER_SIZE];
} buffered_io_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*io_cancel_check_fn)(void *user);

/*
 * Optional cooperative cancellation for metadata-only parser jobs. The check
 * is registered per PSP thread, so a cancelled browser probe cannot affect
 * playback or another worker. A cancelled reader advances to EOF so legacy
 * parser loops terminate instead of spinning on a non-advancing position.
 */
void io_set_cancel_check(io_cancel_check_fn check, void *user);

int32_t io_open64(const char* filename, void* handle);
int64_t io_set_position64(void* handle, int64_t position);
int64_t io_get_position64(void* handle);
int64_t io_get_length64(void* handle);
/* Legacy calls are checked wrappers, never implicit narrowing conversions. */
int32_t io_open(const char* filename, void* handle);
int32_t io_set_position(void* handle, const int32_t position);
int32_t io_get_position(void* handle);
int32_t io_get_length(void* handle);
uint32_t io_read_data(void* handle, uint8_t* data, const uint32_t size);
uint64_t io_read_be64(void* handle);
uint32_t io_read_be32(void* handle);
uint32_t io_read_be24(void* handle);
uint16_t io_read_be16(void* handle);
uint8_t io_read_8(void* handle);
void io_close(void* handle);

#ifdef __cplusplus
}
#endif

#endif
