#include "ppa_vfs.h"
#include "ppa_ntfs.h"
#include "ppa_io.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pspiofilemgr.h>

#define PPA_VFS_BACKEND_DIRECT 0
#define PPA_VFS_BACKEND_NTFS   1

#define PPA_VFS_DIRECT_BURST   (128U * 1024U)
#define PPA_VFS_NTFS_BURST     (32U * 1024U)

struct ppa_vfs_file {
	int backend;
	SceUID fd;
	int64_t size;
	int64_t offset;
	int64_t physical_offset;
	uint32_t capabilities;
	int native_async_disabled;
	ppa_ntfs_file *ntfs;
	char path[512];
};

static int ppa_vfs_has_prefix(const char *path, const char *prefix)
{
	return path != 0 && prefix != 0 &&
	       strncasecmp(path, prefix, strlen(prefix)) == 0;
}

static uint32_t ppa_vfs_capabilities_for_path(const char *path, int ntfs)
{
	uint32_t caps = PPA_VFS_CAP_POSITIONED_READ;

	if (ntfs) {
		return caps |
		       PPA_VFS_CAP_EXPENSIVE_SEEK |
		       PPA_VFS_CAP_RAW_CACHE_BACKED |
		       PPA_VFS_CAP_REMOVABLE_MEDIA;
	}

	if (ppa_vfs_has_prefix(path, "ms0:") ||
	    ppa_vfs_has_prefix(path, "ef0:")) {
		caps |= PPA_VFS_CAP_ASYNC_READ;
		caps |= PPA_VFS_CAP_REMOVABLE_MEDIA;
	}

	return caps;
}

int ppa_vfs_path_is_supported(const char *path)
{
	if (path == 0 || path[0] == 0)
		return 1; /* Empty path means the explicit browser root. */

	/* UMD/ISO devices are deliberately outside PPA's playback contract. */
	if (ppa_vfs_has_prefix(path, "umd0:") ||
	    ppa_vfs_has_prefix(path, "disc0:") ||
	    ppa_vfs_has_prefix(path, "isofs0:"))
		return 0;

	return ppa_vfs_has_prefix(path, "ms0:") ||
	       ppa_vfs_has_prefix(path, "ms1:") ||
	       ppa_vfs_has_prefix(path, "ef0:");
}

int ppa_vfs_init(void)
{
	int ntfs_ok;

	ntfs_ok = ppa_ntfs_init();
	return ntfs_ok;
}

void ppa_vfs_shutdown(void)
{
	ppa_ntfs_shutdown();
}

int ppa_vfs_is_ntfs_path(const char *path)
{
	return ppa_ntfs_path_is_ntfs(path);
}

int ppa_vfs_ntfs_available(void)
{
	return ppa_ntfs_available();
}

int ppa_vfs_open_directory(const char *path,
                           int show_hidden,
                           int show_unknown,
                           file_type_ext_struct *filter,
                           directory_item_struct **items)
{
	char short_path[512];
	const char *safe_path = path ? path : "";
	size_t path_length;

	if (!ppa_vfs_path_is_supported(safe_path)) {
		return 0;
	}

	if (ppa_ntfs_path_is_ntfs(safe_path))
		return ppa_ntfs_readdir(safe_path, show_hidden, show_unknown,
		                        filter, 0, items);

	path_length = strlen(safe_path);
	if (path_length >= sizeof(short_path)) {
		return 0;
	}
	memcpy(short_path, safe_path, path_length + 1U);
	return open_directory(safe_path, short_path, show_hidden, show_unknown,
	                      filter, items);
}

ppa_vfs_file *ppa_vfs_open(const char *path)
{
	ppa_vfs_file *file;
	int ntfs;

	if (path == 0 || path[0] == 0 || strlen(path) >= sizeof(file->path) ||
	    !ppa_vfs_path_is_supported(path)) {
		return 0;
	}

	ntfs = ppa_ntfs_path_is_ntfs(path);

	file = (ppa_vfs_file *)malloc(sizeof(*file));
	if (file == 0)
		return 0;

	memset(file, 0, sizeof(*file));
	file->fd = -1;
	file->size = -1;
	file->offset = 0;
	file->physical_offset = 0;
	file->capabilities = ppa_vfs_capabilities_for_path(path, ntfs);
	strncpy(file->path, path, sizeof(file->path) - 1U);
	file->path[sizeof(file->path) - 1U] = 0;

	if (ntfs) {
		file->backend = PPA_VFS_BACKEND_NTFS;
		file->ntfs = ppa_ntfs_open(path);
		if (file->ntfs == 0) {
			free(file);
			return 0;
		}

		file->size = ppa_ntfs_size64(file->ntfs);
		if (file->size < 0) {
			ppa_ntfs_close(file->ntfs);
			free(file);
			return 0;
		}

		return file;
	}

	file->backend = PPA_VFS_BACKEND_DIRECT;
	file->fd = ppa_io_open_read_retry(path, PSP_O_RDONLY, 0777);
	if (file->fd < 0) {
		free(file);
		return 0;
	}

	file->size = (int64_t)ppa_io_seek_retry(file->fd, 0, PSP_SEEK_END);
	if (file->size < 0) {
		sceIoClose(file->fd);
		free(file);
		return 0;
	}

	if (ppa_io_seek_retry(file->fd, 0, PSP_SEEK_SET) != 0) {
		sceIoClose(file->fd);
		free(file);
		return 0;
	}

	return file;
}

void ppa_vfs_close(ppa_vfs_file *file)
{
	if (file == 0)
		return;

	if (file->backend == PPA_VFS_BACKEND_NTFS)
		ppa_ntfs_close(file->ntfs);
	else if (file->fd >= 0)
		sceIoClose(file->fd);

	free(file);
}

uint32_t ppa_vfs_capabilities(ppa_vfs_file *file)
{
	uint32_t caps;

	if (file == 0)
		return 0;

	caps = file->capabilities;
	if (file->native_async_disabled)
		caps &= ~PPA_VFS_CAP_ASYNC_READ;
	return caps;
}

uint32_t ppa_vfs_recommended_burst(ppa_vfs_file *file)
{
	if (file != 0 && file->backend == PPA_VFS_BACKEND_NTFS)
		return PPA_VFS_NTFS_BURST;
	return PPA_VFS_DIRECT_BURST;
}

int64_t ppa_vfs_size64(ppa_vfs_file *file)
{
	return file != 0 ? file->size : -1;
}

int64_t ppa_vfs_seek64(ppa_vfs_file *file, int64_t position)
{
	int64_t result;

	if (file == 0)
		return -1;
	if (position < 0)
		position = 0;
	if (file->size >= 0 && position > file->size)
		position = file->size;

	if (file->backend == PPA_VFS_BACKEND_NTFS) {
		result = ppa_ntfs_seek64(file->ntfs, position);
		if (result >= 0)
			file->offset = result;
		return result;
	}

	result = (int64_t)ppa_io_seek_retry(file->fd, (SceOff)position,
	                                   PSP_SEEK_SET);
	if (result >= 0) {
		file->offset = result;
		file->physical_offset = result;
	}
	return result;
}

int64_t ppa_vfs_tell64(ppa_vfs_file *file)
{
	return file != 0 ? file->offset : -1;
}

/* Firmware reads return signed int counts. Keep all backends within that
 * range and within the opened file before pointer/offset arithmetic occurs. */
static int ppa_vfs_bounded_read_size(ppa_vfs_file *file,
                                    uint64_t position,
                                    unsigned int requested)
{
	uint64_t remaining;
	if (file == 0 || file->size < 0 || position > (uint64_t)INT64_MAX ||
	    requested > (unsigned int)INT_MAX)
		return -1;
	if (position >= (uint64_t)file->size)
		return 0;
	remaining = (uint64_t)file->size - position;
	return remaining < requested ? (int)remaining : (int)requested;
}

int ppa_vfs_read(ppa_vfs_file *file, void *buffer, unsigned int size)
{
	int result;
	int bounded;

	if (file == 0 || buffer == 0)
		return -1;
	bounded = ppa_vfs_bounded_read_size(file, (uint64_t)file->offset, size);
	if (bounded <= 0)
		return bounded;
	size = (unsigned int)bounded;

	if (file->backend == PPA_VFS_BACKEND_NTFS) {
		result = ppa_ntfs_read(file->ntfs, buffer, size);
		if (result > 0)
			file->offset += result;
		return result;
	}

	if (file->physical_offset != file->offset) {
		if ((int64_t)ppa_io_seek_retry(file->fd, (SceOff)file->offset,
		                               PSP_SEEK_SET) != file->offset)
			return -1;
		file->physical_offset = file->offset;
	}

	result = ppa_io_read_retry(file->fd, buffer, size);
	if (result > 0) {
		file->offset += result;
		file->physical_offset += result;
	}
	return result;
}

int ppa_vfs_stream_read_at64(ppa_vfs_file *file,
                             uint64_t position,
                             void *buffer,
                             unsigned int size)
{
	int result;
	int bounded;

	if (buffer == 0)
		return -1;
	bounded = ppa_vfs_bounded_read_size(file, position, size);
	if (bounded <= 0)
		return bounded;
	size = (unsigned int)bounded;

	if (file->backend == PPA_VFS_BACKEND_NTFS)
		return ppa_ntfs_read_at64(file->ntfs, position, buffer, size);

	if (file->physical_offset != (int64_t)position) {
		if ((uint64_t)ppa_io_seek_retry(file->fd, (SceOff)position,
		                                PSP_SEEK_SET) != position)
			return -1;
		file->physical_offset = (int64_t)position;
	}

	result = ppa_io_read_retry(file->fd, buffer, size);
	if (result > 0)
		file->physical_offset += result;
	return result;
}

int ppa_vfs_read_at64(ppa_vfs_file *file,
                      uint64_t position,
                      void *buffer,
                      unsigned int size)
{
	int64_t old_offset;
	int result;

	if (file == 0 || buffer == 0 || position > (uint64_t)INT64_MAX)
		return -1;
	old_offset = file->offset;
	result = ppa_vfs_stream_read_at64(file, position, buffer, size);

	/* Preserve cursor semantics for legacy callers. Window owners deliberately
	 * use stream/worker reads and avoid this restore seek. */
	if (file->backend == PPA_VFS_BACKEND_DIRECT && old_offset >= 0 &&
	    file->physical_offset != old_offset) {
		if ((int64_t)ppa_io_seek_retry(file->fd, (SceOff)old_offset,
		                               PSP_SEEK_SET) == old_offset)
			file->physical_offset = old_offset;
	}
	file->offset = old_offset;
	return result;
}

int ppa_vfs_worker_read_at64(ppa_vfs_file *file,
                             uint64_t position,
                             void *buffer,
                             unsigned int size,
                             int prefer_native_async,
                             int *out_used_native_async,
                             int *out_async_fallback)
{
	int result;
	int bounded;
	int fallback = 0;
	uint32_t caps;

	if (out_used_native_async != 0)
		*out_used_native_async = 0;
	if (out_async_fallback != 0)
		*out_async_fallback = 0;
	if (file == 0 || buffer == 0 || position > (uint64_t)INT64_MAX)
		return -1;
	bounded = ppa_vfs_bounded_read_size(file, position, size);
	if (bounded <= 0)
		return bounded;
	size = (unsigned int)bounded;

	caps = ppa_vfs_capabilities(file);
	if (prefer_native_async && file->backend == PPA_VFS_BACKEND_DIRECT &&
	    (caps & PPA_VFS_CAP_ASYNC_READ) != 0U) {
		if (out_used_native_async != 0)
			*out_used_native_async = 1;
		result = ppa_io_async_read_exact_at_retry(file->fd,
		                                           (SceOff)position,
		                                           buffer, size,
		                                           &fallback);
		if (fallback) {
			file->native_async_disabled = 1;
			if (out_async_fallback != 0)
				*out_async_fallback = 1;
		}
		if (result > 0)
			file->physical_offset = (int64_t)position + result;
		return result;
	}

	return ppa_vfs_stream_read_at64(file, position, buffer, size);
}

int ppa_vfs_size(ppa_vfs_file *file)
{
	int64_t result = ppa_vfs_size64(file);
	if (result < 0)
		return -1;
	if (result > INT_MAX)
		return INT_MAX;
	return (int)result;
}

int ppa_vfs_seek(ppa_vfs_file *file, int position)
{
	int64_t result = ppa_vfs_seek64(file, (int64_t)position);
	if (result < 0)
		return -1;
	if (result > INT_MAX)
		return INT_MAX;
	return (int)result;
}

int ppa_vfs_tell(ppa_vfs_file *file)
{
	int64_t result = ppa_vfs_tell64(file);
	if (result < 0)
		return -1;
	if (result > INT_MAX)
		return INT_MAX;
	return (int)result;
}

int ppa_vfs_stream_read_at(ppa_vfs_file *file, int position,
                           void *buffer, unsigned int size)
{
	if (position < 0)
		return -1;
	return ppa_vfs_stream_read_at64(file, (uint64_t)(unsigned int)position,
	                                buffer, size);
}

int ppa_vfs_read_at(ppa_vfs_file *file, int position,
                    void *buffer, unsigned int size)
{
	if (position < 0)
		return -1;
	return ppa_vfs_read_at64(file, (uint64_t)(unsigned int)position,
	                         buffer, size);
}
