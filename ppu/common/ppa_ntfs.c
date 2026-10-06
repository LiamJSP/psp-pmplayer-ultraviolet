#include "ppa_ntfs.h"
#include "ppa_ms_partition.h"
#include "ppa_io.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <pspiofilemgr.h>

#include <libfsntfs.h>
#include <libbfio.h>

#ifndef stricmp
#define stricmp strcasecmp
#endif

#define PPA_NTFS_RAW_SECTOR_SIZE 512ULL
/*
 * libbfio/libfsntfs treats a short custom-handle callback as an I/O failure in
 * several non-resident data paths.  Keep the callback and file-entry request
 * granularity at the original, known-good 32 KiB contract.  Physical Memory
 * Stick read-ahead is deliberately separate: the raw cache may still fetch
 * 64/128 KiB and satisfy several 32 KiB callbacks without another media read.
 */
#define PPA_NTFS_RAW_CALLBACK_MAX_READ_SIZE (32U * 1024U)
#define PPA_NTFS_RAW_RANDOM_FILL_SIZE (64U * 1024U)
#define PPA_NTFS_RAW_CACHE_SIZE (128U * 1024U)
#define PPA_NTFS_FILE_READ_CHUNK_SIZE (32U * 1024U)
#define PPA_NTFS_DIR_GROW_COUNT 64
#define PPA_NTFS_MAX_DIR_ENTRIES 2048

#define PPA_NTFS_FILE_ATTRIBUTE_READONLY  0x00000001U
#define PPA_NTFS_FILE_ATTRIBUTE_HIDDEN    0x00000002U
#define PPA_NTFS_FILE_ATTRIBUTE_SYSTEM    0x00000004U
#define PPA_NTFS_FILE_ATTRIBUTE_DIRECTORY 0x00000010U
#define PPA_NTFS_FILE_ATTRIBUTE_REPARSE   0x00000400U
#define PPA_NTFS_ATTRIBUTE_TYPE_REPARSE_POINT 0x000000C0U

#if defined(PPA_ENABLE_NTFS)

#include <libfsntfs.h>
#include <libbfio.h>

struct ppa_ntfs_file {
	libfsntfs_file_entry_t *entry;
	size64_t size;
	off64_t offset;
	char path[PPA_NTFS_MAX_PATH];
};

static libfsntfs_volume_t *g_ntfs_volume = 0;
static libbfio_handle_t *g_ntfs_bfio = 0;
static int g_ntfs_available = 0;

static void ppa_ntfs_free_fs_error(libfsntfs_error_t **error);

static u32 ppa_ntfs_filetype_from_name(const char *name)
{
	const char *ext;

	if (name == 0)
		return FS_UNKNOWN_FILE;

	ext = strrchr(name, '.');
	if (ext == 0)
		return FS_UNKNOWN_FILE;

	ext++;

	if (stricmp(ext, "mp4") == 0)
		return FS_MP4_FILE;
	if (stricmp(ext, "mkv") == 0)
		return FS_MKV_FILE;
	if (stricmp(ext, "srt") == 0)
		return FS_SRT_FILE;
	if (stricmp(ext, "sub") == 0)
		return FS_SUB_FILE;
	if (stricmp(ext, "ass") == 0)
		return FS_ASS_FILE;
	if (stricmp(ext, "ssa") == 0)
		return FS_SSA_FILE;
	if (stricmp(ext, "png") == 0)
		return FS_PNG_FILE;

	return FS_UNKNOWN_FILE;
}

static int ppa_ntfs_filter_accept(file_type_ext_struct *filter,
                                  const char *name,
                                  u32 filetype,
                                  int show_unknown)
{
	file_type_ext_struct *f;

	if (filetype == FS_DIRECTORY)
		return 1;

	if (filter == 0)
		return show_unknown || filetype != FS_UNKNOWN_FILE;

	for (f = filter; f->ext != 0; f++) {
		if (f->filetype == filetype)
			return 1;
	}

	if (show_unknown && filetype == FS_UNKNOWN_FILE)
		return 1;

	(void)name;
	return 0;
}

int ppa_ntfs_path_is_ntfs(const char *path)
{
	return path != 0 &&
	       path[0] == 'm' &&
	       path[1] == 's' &&
	       path[2] == '1' &&
	       path[3] == ':';
}

const char *ppa_ntfs_strip_prefix(const char *path)
{
	if (!ppa_ntfs_path_is_ntfs(path))
		return path;

	path += 4;

	if (*path == '/')
		path++;

	return path;
}

static int ppa_ntfs_path_is_volume_root(const char *path)
{
	const char *relative;

	if (!ppa_ntfs_path_is_ntfs(path))
		return 0;

	relative = ppa_ntfs_strip_prefix(path);
	if (relative == 0)
		return 1;

	while (*relative == '/' || *relative == '\\')
		relative++;

	return *relative == 0;
}

typedef struct ppa_ntfs_raw_handle {
	SceUID fd;
	uint64_t partition_offset;
	uint64_t partition_size;
	uint64_t current_offset;
	uint8_t *read_cache;
	uint64_t read_cache_offset;
	unsigned int read_cache_size;
	unsigned int read_cache_capacity;
	uint64_t last_request_end;
} ppa_ntfs_raw_handle;

static ppa_ntfs_raw_handle *g_ntfs_raw = 0;

static int ppa_ntfs_raw_open(intptr_t *io_handle,
                             int access_flags,
                             libcerror_error_t **error)
{
	ppa_ntfs_raw_handle *raw = (ppa_ntfs_raw_handle *)io_handle;

	(void)access_flags;
	(void)error;

	if (raw == 0)
		return -1;

	if (raw->fd >= 0)
		return 1;

	raw->fd = ppa_io_open_read_retry("msstor:", PSP_O_RDONLY, 0777);

	if (raw->fd < 0) {
		return -1;
	}

	raw->current_offset = 0;
	raw->read_cache_size = 0;
	raw->last_request_end = 0;

	return 1;
}

static int ppa_ntfs_raw_close(intptr_t *io_handle,
                              libcerror_error_t **error)
{
	ppa_ntfs_raw_handle *raw = (ppa_ntfs_raw_handle *)io_handle;

	(void)error;

	if (raw == 0)
		return -1;

	if (raw->fd >= 0) {
		sceIoClose(raw->fd);
		raw->fd = -1;
	}

	return 1;
}

static int ppa_ntfs_raw_read_physical_aligned(ppa_ntfs_raw_handle *raw,
                                              uint64_t physical_offset,
                                              uint8_t *buffer,
                                              size_t size)
{
	uint64_t request_end;
	uint64_t partition_end;
	uint64_t cache_end;
	uint64_t fill_end;
	uint64_t minimum_fill;
	unsigned int fill_size;
	unsigned int fill_capacity;
	int sequential;
	int result;

	if (raw == 0 || buffer == 0)
		return -1;
	if (size == 0)
		return 0;
	if (size > PPA_NTFS_RAW_CALLBACK_MAX_READ_SIZE ||
	    physical_offset > UINT64_MAX - (uint64_t)size)
		return -1;

	request_end = physical_offset + (uint64_t)size;
	partition_end = raw->partition_offset + raw->partition_size;
	if (partition_end < raw->partition_offset ||
	    physical_offset < raw->partition_offset ||
	    request_end > partition_end)
		return -1;

	cache_end = raw->read_cache_offset + raw->read_cache_size;
	if (raw->read_cache != 0 &&
	    cache_end >= raw->read_cache_offset &&
	    physical_offset >= raw->read_cache_offset &&
	    request_end <= cache_end) {
		memcpy(buffer,
		       raw->read_cache + (unsigned int)(physical_offset -
		                                        raw->read_cache_offset),
		       size);
		raw->last_request_end = request_end;
		return (int)size;
	}

	if (raw->read_cache == 0) {
		raw->read_cache = (uint8_t *)malloc(PPA_NTFS_RAW_CACHE_SIZE);
		if (raw->read_cache != 0)
			raw->read_cache_capacity = PPA_NTFS_RAW_CACHE_SIZE;
	}

	if (raw->read_cache != 0) {
		/* Cache starts at a sector boundary and reads ahead across adjacent
		 * NTFS metadata/data requests. A cold/random metadata miss reads 64 KiB;
		 * a confirmed forward miss expands to 128 KiB. This avoids unnecessary
		 * media traffic for sparse MFT/runlist probes while letting sequential
		 * payload reads amortize controller wakeups. */
		sequential = (raw->last_request_end == physical_offset) ||
		             (raw->read_cache_size != 0U &&
		              cache_end >= raw->read_cache_offset &&
		              physical_offset == cache_end);
		fill_capacity = sequential ? raw->read_cache_capacity :
		                PPA_NTFS_RAW_RANDOM_FILL_SIZE;
		raw->read_cache_offset =
			physical_offset & ~(PPA_NTFS_RAW_SECTOR_SIZE - 1ULL);
		minimum_fill = request_end - raw->read_cache_offset;
		if (minimum_fill > UINT64_MAX -
		                   (PPA_NTFS_RAW_SECTOR_SIZE - 1ULL))
			return -1;
		minimum_fill = (minimum_fill + PPA_NTFS_RAW_SECTOR_SIZE - 1ULL) &
		               ~(PPA_NTFS_RAW_SECTOR_SIZE - 1ULL);
		if (minimum_fill > raw->read_cache_capacity)
			return -1;
		if ((uint64_t)fill_capacity < minimum_fill)
			fill_capacity = (unsigned int)minimum_fill;
		fill_end = raw->read_cache_offset + fill_capacity;
		if (fill_end < raw->read_cache_offset || fill_end > partition_end)
			fill_end = partition_end;
		fill_size = (unsigned int)(fill_end - raw->read_cache_offset);
		if (fill_size == 0 || request_end > fill_end)
			return -1;

		result = ppa_io_read_exact_at_retry(raw->fd,
		                                   (SceOff)raw->read_cache_offset,
		                                   raw->read_cache,
		                                   fill_size);
		if (result != (int)fill_size) {
			raw->read_cache_size = 0;
			return -1;
		}
		raw->read_cache_size = fill_size;
		memcpy(buffer,
		       raw->read_cache + (unsigned int)(physical_offset -
		                                        raw->read_cache_offset),
		       size);
		raw->last_request_end = request_end;
		return (int)size;
	}

	/* Low-memory fallback preserves the original exact sector-aligned
	 * semantics without changing libfsntfs-visible behavior. */
	if (request_end > UINT64_MAX - (PPA_NTFS_RAW_SECTOR_SIZE - 1ULL))
		return -1;
	{
		uint64_t aligned_offset =
			physical_offset & ~(PPA_NTFS_RAW_SECTOR_SIZE - 1ULL);
		uint64_t leading = physical_offset - aligned_offset;
		uint64_t aligned_end =
			(request_end + PPA_NTFS_RAW_SECTOR_SIZE - 1ULL) &
			~(PPA_NTFS_RAW_SECTOR_SIZE - 1ULL);
		unsigned int aligned_size = (unsigned int)(aligned_end - aligned_offset);
		uint8_t *bounce = (uint8_t *)malloc(aligned_size);
		if (bounce == 0)
			return -1;
		result = ppa_io_read_exact_at_retry(raw->fd, (SceOff)aligned_offset,
		                                   bounce, aligned_size);
		if (result == (int)aligned_size) {
			memcpy(buffer, bounce + leading, size);
			raw->last_request_end = request_end;
		}
		free(bounce);
		return result == (int)aligned_size ? (int)size : -1;
	}
}

static ssize_t ppa_ntfs_raw_read_buffer(intptr_t *io_handle,
                                        uint8_t *buffer,
                                        size_t size,
                                        libcerror_error_t **error)
{
	ppa_ntfs_raw_handle *raw = (ppa_ntfs_raw_handle *)io_handle;
	uint64_t physical_offset;
	uint64_t remaining;
	size_t read_size;
	int result;

	(void)error;

	if (raw == 0 || buffer == 0)
		return -1;

	if (size == 0)
		return 0;

	if (raw->fd < 0) {
		if (ppa_ntfs_raw_open(io_handle, 0, error) != 1)
			return -1;
	}

	if (raw->current_offset >= raw->partition_size)
		return 0;

	remaining = raw->partition_size - raw->current_offset;
	read_size = size;

	if ((uint64_t)read_size > remaining)
		read_size = (size_t)remaining;

	if (read_size > PPA_NTFS_RAW_CALLBACK_MAX_READ_SIZE)
		read_size = PPA_NTFS_RAW_CALLBACK_MAX_READ_SIZE;

	if (read_size == 0)
		return 0;

	if (raw->partition_offset > UINT64_MAX - raw->current_offset)
		return -1;

	physical_offset = raw->partition_offset + raw->current_offset;

	result = ppa_ntfs_raw_read_physical_aligned(raw,
	                                           physical_offset,
	                                           buffer,
	                                           read_size);

	if (result < 0) {
		return -1;
	}

	raw->current_offset += (uint64_t)result;

	return result;
}

static ssize_t ppa_ntfs_raw_write_buffer(intptr_t *io_handle,
                                         const uint8_t *buffer,
                                         size_t size,
                                         libcerror_error_t **error)
{
	(void)io_handle;
	(void)buffer;
	(void)size;
	(void)error;

	/*
	 * NTFS support in PPA is intentionally read-only.
	 */
	errno = EROFS;
	return -1;
}

static off64_t ppa_ntfs_raw_seek_offset(intptr_t *io_handle,
                                        off64_t offset,
                                        int whence,
                                        libcerror_error_t **error)
{
	ppa_ntfs_raw_handle *raw = (ppa_ntfs_raw_handle *)io_handle;
	int64_t base;
	int64_t target;

	(void)error;

	if (raw == 0)
		return -1;

	if (whence == PSP_SEEK_SET) {
		base = 0;
	} else if (whence == PSP_SEEK_CUR) {
		base = (int64_t)raw->current_offset;
	} else if (whence == PSP_SEEK_END) {
		base = (int64_t)raw->partition_size;
	} else {
		return -1;
	}

	if ((offset > 0 && base > INT64_MAX - (int64_t)offset) ||
	    (offset < 0 && base < INT64_MIN - (int64_t)offset))
		return -1;

	target = base + (int64_t)offset;

	if (target < 0)
		return -1;

	if ((uint64_t)target > raw->partition_size)
		return -1;

	raw->current_offset = (uint64_t)target;

	return (off64_t)raw->current_offset;
}

static int ppa_ntfs_raw_exists(intptr_t *io_handle,
                               libcerror_error_t **error)
{
	(void)io_handle;
	(void)error;

	return 1;
}

static int ppa_ntfs_raw_is_open(intptr_t *io_handle,
                                libcerror_error_t **error)
{
	ppa_ntfs_raw_handle *raw = (ppa_ntfs_raw_handle *)io_handle;

	(void)error;

	if (raw == 0)
		return -1;

	return raw->fd >= 0 ? 1 : 0;
}

static int ppa_ntfs_raw_get_size(intptr_t *io_handle,
                                 size64_t *size,
                                 libcerror_error_t **error)
{
	ppa_ntfs_raw_handle *raw = (ppa_ntfs_raw_handle *)io_handle;

	(void)error;

	if (raw == 0 || size == 0)
		return -1;

	*size = (size64_t)raw->partition_size;

	return 1;
}

static int ppa_ntfs_raw_clone(intptr_t **destination_io_handle,
                              intptr_t *source_io_handle,
                              libcerror_error_t **error)
{
	ppa_ntfs_raw_handle *source = (ppa_ntfs_raw_handle *)source_io_handle;
	ppa_ntfs_raw_handle *clone;

	(void)error;

	if (destination_io_handle == 0 || source == 0)
		return -1;

	clone = (ppa_ntfs_raw_handle *)malloc(sizeof(*clone));

	if (clone == 0)
		return -1;

	memcpy(clone, source, sizeof(*clone));

	/*
	 * Do not share the SceUID across clones.
	 * Reopen lazily if the clone is ever used.
	 */
	clone->fd = -1;
	clone->read_cache = 0;
	clone->read_cache_offset = 0;
	clone->read_cache_size = 0;
	clone->read_cache_capacity = 0;
	clone->last_request_end = 0;

	*destination_io_handle = (intptr_t *)clone;

	return 1;
}

static int ppa_ntfs_raw_free(intptr_t **io_handle,
                             libcerror_error_t **error)
{
	ppa_ntfs_raw_handle *raw;

	(void)error;

	if (io_handle == 0)
		return -1;

	raw = (ppa_ntfs_raw_handle *)(*io_handle);

	if (raw != 0) {
		if (raw->fd >= 0) {
			sceIoClose(raw->fd);
			raw->fd = -1;
		}

		if (raw->read_cache != 0) {
			free(raw->read_cache);
			raw->read_cache = 0;
		}

		if (g_ntfs_raw == raw)
			g_ntfs_raw = 0;

		free(raw);
		*io_handle = 0;
	}

	return 1;
}

static int ppa_ntfs_bfio_open_for_partition(const ppa_ms_partition *partition)
{
	if (partition == 0)
		return 0;

	if (partition->byte_offset == 0 || partition->byte_size == 0)
		return 0;

	g_ntfs_raw = (ppa_ntfs_raw_handle *)malloc(sizeof(*g_ntfs_raw));

	if (g_ntfs_raw == 0)
		return 0;

	memset(g_ntfs_raw, 0, sizeof(*g_ntfs_raw));

	g_ntfs_raw->fd = -1;
	g_ntfs_raw->partition_offset = (uint64_t)partition->byte_offset;
	g_ntfs_raw->partition_size = (uint64_t)partition->byte_size;
	g_ntfs_raw->current_offset = 0;

	if (libbfio_handle_initialize(
	        &g_ntfs_bfio,
	        (intptr_t *)g_ntfs_raw,
	        ppa_ntfs_raw_free,
	        ppa_ntfs_raw_clone,
	        ppa_ntfs_raw_open,
	        ppa_ntfs_raw_close,
	        ppa_ntfs_raw_read_buffer,
	        ppa_ntfs_raw_write_buffer,
	        ppa_ntfs_raw_seek_offset,
	        ppa_ntfs_raw_exists,
	        ppa_ntfs_raw_is_open,
	        ppa_ntfs_raw_get_size,
	        0,
	        NULL) != 1) {
		ppa_ntfs_raw_free((intptr_t **)&g_ntfs_raw, NULL);
		g_ntfs_bfio = 0;
		return 0;
	}

	return 1;
}

static void ppa_ntfs_free_fs_error(libfsntfs_error_t **error)
{
	if (error != 0 && *error != 0)
		libfsntfs_error_free(error);
}

int ppa_ntfs_init(void)
{
	ppa_ms_partition ntfs_partition;
	libfsntfs_error_t *fs_error = 0;
	int access_flags;

	ppa_ntfs_shutdown();

	if (!ppa_ms_partition_find_ntfs(&ntfs_partition)) {
		return 0;
	}

	if (!ppa_ntfs_bfio_open_for_partition(&ntfs_partition)) {
		return 0;
	}

	if (libfsntfs_volume_initialize(&g_ntfs_volume, &fs_error) != 1) {
		ppa_ntfs_free_fs_error(&fs_error);
		ppa_ntfs_shutdown();
		return 0;
	}

	access_flags = libfsntfs_get_access_flags_read();

	if (libfsntfs_volume_open_file_io_handle(g_ntfs_volume,
	                                        g_ntfs_bfio,
	                                        access_flags,
	                                        &fs_error) != 1) {
		ppa_ntfs_free_fs_error(&fs_error);
		ppa_ntfs_shutdown();
		return 0;
	}

	libfsntfs_set_codepage(0, &fs_error);
	ppa_ntfs_free_fs_error(&fs_error);

	g_ntfs_available = 1;

	return 1;
}

void ppa_ntfs_shutdown(void)
{
	libfsntfs_error_t *fs_error = 0;

	g_ntfs_available = 0;

	if (g_ntfs_volume != 0) {
		libfsntfs_volume_close(g_ntfs_volume, &fs_error);
		ppa_ntfs_free_fs_error(&fs_error);

		libfsntfs_volume_free(&g_ntfs_volume, &fs_error);
		ppa_ntfs_free_fs_error(&fs_error);

		g_ntfs_volume = 0;
	}

	if (g_ntfs_bfio != 0) {
		libbfio_handle_close(g_ntfs_bfio, NULL);
		libbfio_handle_free(&g_ntfs_bfio, NULL);
		g_ntfs_bfio = 0;
	}

	if (g_ntfs_raw != 0) {
		ppa_ntfs_raw_free((intptr_t **)&g_ntfs_raw, NULL);
		g_ntfs_raw = 0;
	}
}

int ppa_ntfs_available(void)
{
	return g_ntfs_available;
}
static int ppa_ntfs_entry_name(libfsntfs_file_entry_t *entry,
                               char *out,
                               size_t out_size)
{
	libfsntfs_error_t *error = 0;
	size_t name_size = 0;

	if (entry == 0 || out == 0 || out_size == 0)
		return 0;

	out[0] = 0;

	if (libfsntfs_file_entry_get_utf8_name_size(entry, &name_size, &error) != 1) {
		ppa_ntfs_free_fs_error(&error);
		return 0;
	}

	ppa_ntfs_free_fs_error(&error);

	/*
	 * Do not silently truncate: truncated names can collide, and the
	 * browser later uses compname to build paths.
	 */
	if (name_size == 0 || name_size > out_size)
		return 0;

	if (libfsntfs_file_entry_get_utf8_name(entry,
	                                      (uint8_t *)out,
	                                      name_size,
	                                      &error) != 1) {
		ppa_ntfs_free_fs_error(&error);
		out[0] = 0;
		return 0;
	}

	ppa_ntfs_free_fs_error(&error);
	out[out_size - 1] = 0;

	return out[0] != 0;
}

static int ppa_ntfs_component_is_safe(const char *name)
{
	const unsigned char *p;

	if (name == 0 || name[0] == 0)
		return 0;

	/*
	 * PPA has no UI for NTFS alternate data streams and path lookup uses
	 * '/' and '\\' as separators. Reject ambiguous or control-containing
	 * components early instead of letting them reach libfsntfs.
	 */
	for (p = (const unsigned char *)name; *p != 0; p++) {
		if (*p < 0x20 || *p == ':' || *p == '/' || *p == '\\')
			return 0;
	}

	return 1;
}

static int ppa_ntfs_name_is_browser_hidden(const char *name)
{
	if (name == 0 || name[0] == 0)
		return 1;

	if (strcmp(name, ".") == 0)
		return 1;

	/*
	 * Hide NTFS metadata files ($MFT, $Bitmap, etc.) from the media
	 * browser. They should not be opened by a video player.
	 */
	if (name[0] == '$')
		return 1;

	return !ppa_ntfs_component_is_safe(name);
}

static int ppa_ntfs_relative_path_is_safe(const char *relative)
{
	const char *cursor;

	if (relative == 0)
		return 0;

	cursor = relative;

	while (*cursor != 0) {
		char component[256];
		const char *next;
		size_t component_len;

		while (*cursor == '/' || *cursor == '\\')
			cursor++;

		if (*cursor == 0)
			break;

		next = cursor;
		while (*next != 0 && *next != '/' && *next != '\\')
			next++;

		component_len = (size_t)(next - cursor);
		if (component_len == 0 || component_len >= sizeof(component))
			return 0;

		memcpy(component, cursor, component_len);
		component[component_len] = 0;

		if (strcmp(component, ".") == 0) {
			cursor = next;
			continue;
		}

		if (strcmp(component, "..") == 0)
			return 0;

		if (!ppa_ntfs_component_is_safe(component))
			return 0;

		cursor = next;
	}

	return 1;
}

static int ppa_ntfs_get_entry_flags(libfsntfs_file_entry_t *entry,
                                    uint32_t *flags)
{
	libfsntfs_error_t *error = 0;
	uint32_t local_flags = 0;
	int have_flags = 0;

	if (flags == 0)
		return 0;

	*flags = 0;

	if (entry == 0)
		return 0;

	/*
	 * Directory listings in NTFS carry attribute flags in the parent
	 * directory's $I30/$FILE_NAME entry.  The $STANDARD_INFORMATION flags
	 * are still useful, but damaged or partially cached entries can fail to
	 * expose them.  Merge both sources instead of treating either source as
	 * authoritative by itself; this is especially important for the
	 * DIRECTORY bit.
	 */
	if (libfsntfs_file_entry_get_file_attribute_flags(entry,
	                                                 &local_flags,
	                                                 &error) == 1) {
		*flags |= local_flags;
		have_flags = 1;
	}

	ppa_ntfs_free_fs_error(&error);
	error = 0;
	local_flags = 0;

	if (libfsntfs_file_entry_get_i30_file_attribute_flags(entry,
	                                                     &local_flags,
	                                                     &error) == 1) {
		*flags |= local_flags;
		have_flags = 1;
	}

	ppa_ntfs_free_fs_error(&error);

	return have_flags;
}

static int ppa_ntfs_get_listing_size_legacy(libfsntfs_file_entry_t *entry,
                                            size64_t *out_size)
{
	libfsntfs_error_t *error = 0;
	uint64_t i30_size = 0;
	size64_t data_size = 0;

	if (out_size == 0)
		return 0;

	*out_size = 0;

	if (entry == 0)
		return 0;

	/*
	 * Preserve the original, known-good volume-root behavior.  The root
	 * directory entries exposed by this PSP libfsntfs build are not always
	 * complete $I30 records, so fall back to the reopened entry's default
	 * stream size when the index size is unavailable.
	 */
	if (libfsntfs_file_entry_get_i30_size(entry, &i30_size, &error) == 1) {
		*out_size = (size64_t)i30_size;
		ppa_ntfs_free_fs_error(&error);
		return 1;
	}

	ppa_ntfs_free_fs_error(&error);
	error = 0;

	if (libfsntfs_file_entry_get_size(entry, &data_size, &error) == 1) {
		*out_size = data_size;
		ppa_ntfs_free_fs_error(&error);
		return 1;
	}

	ppa_ntfs_free_fs_error(&error);
	return 0;
}

static int ppa_ntfs_get_listing_size_fast(libfsntfs_file_entry_t *entry,
                                          size64_t *out_size)
{
	libfsntfs_error_t *error = 0;
	uint64_t i30_size = 0;

	if (out_size == 0)
		return 0;
	*out_size = 0;
	if (entry == 0)
		return 0;

	/* Directory enumeration must not open/parse a large non-resident data
	 * stream merely to paint a file-size column. $I30 already carries the
	 * parent index's logical size and is effectively constant-cost. Exact
	 * stream metadata is loaded lazily only for the selected browser item. */
	if (libfsntfs_file_entry_get_i30_size(entry, &i30_size, &error) == 1) {
		*out_size = (size64_t)i30_size;
		ppa_ntfs_free_fs_error(&error);
		return 1;
	}
	ppa_ntfs_free_fs_error(&error);
	return 0;
}

static int ppa_ntfs_entry_has_reparse_attribute(libfsntfs_file_entry_t *entry)
{
	libfsntfs_error_t *error = 0;
	libfsntfs_attribute_t *attribute = 0;
	int attr_count = 0;
	int i;

	if (entry == 0)
		return 0;

	if (libfsntfs_file_entry_get_number_of_attributes(entry,
	                                                &attr_count,
	                                                &error) != 1) {
		ppa_ntfs_free_fs_error(&error);
		return 0;
	}

	ppa_ntfs_free_fs_error(&error);

	if (attr_count < 0)
		return 0;

	if (attr_count > 128)
		attr_count = 128;

	for (i = 0; i < attr_count; i++) {
		uint32_t attr_type = 0;

		attribute = 0;
		error = 0;

		if (libfsntfs_file_entry_get_attribute_by_index(entry,
		                                             i,
		                                             &attribute,
		                                             &error) != 1) {
			ppa_ntfs_free_fs_error(&error);
			continue;
		}

		if (libfsntfs_attribute_get_type(attribute, &attr_type, &error) == 1 &&
		    attr_type == PPA_NTFS_ATTRIBUTE_TYPE_REPARSE_POINT) {
			libfsntfs_attribute_free(&attribute, &error);
			ppa_ntfs_free_fs_error(&error);
			return 1;
		}

		ppa_ntfs_free_fs_error(&error);
		error = 0;
		libfsntfs_attribute_free(&attribute, &error);
		ppa_ntfs_free_fs_error(&error);
	}

	return 0;
}

static int ppa_ntfs_entry_is_reparse_point(libfsntfs_file_entry_t *entry,
                                           uint32_t flags,
                                           int flags_valid)
{
	libfsntfs_error_t *error = 0;
	int is_link = 0;

	if (entry == 0)
		return 0;

	if (flags_valid && ((flags & PPA_NTFS_FILE_ATTRIBUTE_REPARSE) != 0))
		return 1;

	is_link = libfsntfs_file_entry_is_symbolic_link(entry, &error);
	ppa_ntfs_free_fs_error(&error);

	if (is_link == 1)
		return 1;

	if (!flags_valid)
		return ppa_ntfs_entry_has_reparse_attribute(entry);

	return 0;
}

static int ppa_ntfs_entry_is_directory(libfsntfs_file_entry_t *entry,
                                       uint32_t flags,
                                       int flags_valid)
{
	libfsntfs_error_t *error = 0;
	int has_dir_index = 0;
	int sub_count = -1;
	int has_stream = 0;

	if (entry == 0)
		return 0;

	if (flags_valid && ((flags & PPA_NTFS_FILE_ATTRIBUTE_DIRECTORY) != 0))
		return 1;

	/*
	 * If the attribute flags are missing or stale, the $I30 directory
	 * index is the next best read-only proof that an entry is a directory.
	 */
	has_dir_index = libfsntfs_file_entry_has_directory_entries_index(entry, &error);
	ppa_ntfs_free_fs_error(&error);

	if (has_dir_index == 1)
		return 1;

	if (flags_valid)
		return 0;

	/*
	 * Last-chance corruption tolerance: if flags and the direct index test
	 * fail but enumeration succeeds and there is no default data stream,
	 * treat the entry as a directory.
	 */
	if (libfsntfs_file_entry_get_number_of_sub_file_entries(entry,
	                                                       &sub_count,
	                                                       &error) != 1) {
		ppa_ntfs_free_fs_error(&error);
		return 0;
	}

	ppa_ntfs_free_fs_error(&error);

	error = 0;
	has_stream = libfsntfs_file_entry_has_default_data_stream(entry, &error);
	ppa_ntfs_free_fs_error(&error);

	if (has_stream == 1)
		return 0;

	return sub_count >= 0;
}

static int ppa_ntfs_get_child_entry_by_utf8_name(libfsntfs_file_entry_t *dir,
                                                 const char *name,
                                                 libfsntfs_file_entry_t **out_child);

static int ppa_ntfs_find_child_by_name(libfsntfs_file_entry_t *dir,
                                       const char *target_name,
                                       libfsntfs_file_entry_t **out_child)
{
	libfsntfs_error_t *error = 0;
	libfsntfs_file_entry_t *child = 0;
	int count = 0;
	int i;

	if (out_child == 0)
		return 0;

	*out_child = 0;

	if (dir == 0 || target_name == 0 || target_name[0] == 0)
		return 0;

	if (!ppa_ntfs_component_is_safe(target_name))
		return 0;

	/*
	 * Prefer libfsntfs' named child resolver.  The index-by-number API can
	 * return lightweight directory-index entries; those are usable for display
	 * but can be incomplete when used as the next path component.
	 */
	if (ppa_ntfs_get_child_entry_by_utf8_name(dir, target_name, out_child))
		return 1;

	if (libfsntfs_file_entry_get_number_of_sub_file_entries(dir,
	                                                       &count,
	                                                       &error) != 1) {
		ppa_ntfs_free_fs_error(&error);
		return 0;
	}

	ppa_ntfs_free_fs_error(&error);

	if (count < 0)
		count = 0;

	if (count > PPA_NTFS_MAX_DIR_ENTRIES)
		count = PPA_NTFS_MAX_DIR_ENTRIES;

	for (i = 0; i < count; i++) {
		char name[256];
		if (ppa_io_cancel_requested())
			break;

		child = 0;
		error = 0;

		if (libfsntfs_file_entry_get_sub_file_entry_by_index(dir,
		                                                     i,
		                                                     &child,
		                                                     &error) != 1) {
			ppa_ntfs_free_fs_error(&error);
			continue;
		}

		if (!ppa_ntfs_entry_name(child, name, sizeof(name))) {
			libfsntfs_file_entry_free(&child, &error);
			ppa_ntfs_free_fs_error(&error);
			continue;
		}

		if (strcmp(name, target_name) == 0 || stricmp(name, target_name) == 0) {
			*out_child = child;
			return 1;
		}

		libfsntfs_file_entry_free(&child, &error);
		ppa_ntfs_free_fs_error(&error);
	}

	return 0;
}

static void ppa_ntfs_path_copy_with_separator(char *dst,
                                              size_t dst_size,
                                              const char *src,
                                              char separator,
                                              int leading_separator)
{
	char *out;
	const char *in;
	size_t remaining;

	if (dst == 0 || dst_size == 0)
		return;

	dst[0] = 0;

	if (src == 0)
		return;

	out = dst;
	remaining = dst_size;

	if (leading_separator && remaining > 1) {
		*out++ = separator;
		remaining--;
	}

	for (in = src; *in != 0 && remaining > 1; in++) {
		if (*in == '/' || *in == '\\')
			*out++ = separator;
		else
			*out++ = *in;

		remaining--;
	}

	*out = 0;
}

static int ppa_ntfs_try_get_entry_by_utf8_path(const char *candidate,
                                               libfsntfs_file_entry_t **entry)
{
	libfsntfs_error_t *error = 0;
	libfsntfs_file_entry_t *found = 0;
	size_t candidate_length;
	int result;

	if (candidate == 0 || candidate[0] == 0 || entry == 0)
		return 0;

	candidate_length = strlen(candidate);

	result = libfsntfs_volume_get_file_entry_by_utf8_path(
	             g_ntfs_volume,
	             (const uint8_t *)candidate,
	             candidate_length,
	             &found,
	             &error);
	ppa_ntfs_free_fs_error(&error);

	if (result == 1 && found != 0) {
		*entry = found;
		return 1;
	}

	if (found != 0) {
		libfsntfs_file_entry_free(&found, &error);
		ppa_ntfs_free_fs_error(&error);
		found = 0;
	}

	/* Some libyal builds expect the terminating NUL to be included. */
	result = libfsntfs_volume_get_file_entry_by_utf8_path(
	             g_ntfs_volume,
	             (const uint8_t *)candidate,
	             candidate_length + 1,
	             &found,
	             &error);
	ppa_ntfs_free_fs_error(&error);

	if (result == 1 && found != 0) {
		*entry = found;
		return 1;
	}

	if (found != 0) {
		libfsntfs_file_entry_free(&found, &error);
		ppa_ntfs_free_fs_error(&error);
	}

	return 0;
}

static int ppa_ntfs_get_entry_by_direct_utf8_path(const char *relative,
                                                  libfsntfs_file_entry_t **entry)
{
	char candidate[PPA_NTFS_MAX_PATH + 2];
	char candidate_with_root[PPA_NTFS_MAX_PATH + 2];

	if (entry == 0)
		return 0;

	*entry = 0;

	if (relative == 0 || relative[0] == 0)
		return 0;

	/*
	 * libfsntfs can resolve full UTF-8 paths itself.  Prefer it for nested
	 * directories because sub-file entries returned from a parent directory
	 * can be lightweight directory-index entries on some builds; those entries
	 * are fine for display but may enumerate as empty when used as a directory.
	 */
	ppa_ntfs_path_copy_with_separator(candidate,
	                                  sizeof(candidate),
	                                  relative,
	                                  '/',
	                                  0);

	if (ppa_ntfs_try_get_entry_by_utf8_path(candidate, entry))
		return 1;

	ppa_ntfs_path_copy_with_separator(candidate_with_root,
	                                  sizeof(candidate_with_root),
	                                  relative,
	                                  '/',
	                                  1);

	if (ppa_ntfs_try_get_entry_by_utf8_path(candidate_with_root, entry))
		return 1;

	ppa_ntfs_path_copy_with_separator(candidate,
	                                  sizeof(candidate),
	                                  relative,
	                                  '\\',
	                                  0);

	if (ppa_ntfs_try_get_entry_by_utf8_path(candidate, entry))
		return 1;

	ppa_ntfs_path_copy_with_separator(candidate_with_root,
	                                  sizeof(candidate_with_root),
	                                  relative,
	                                  '\\',
	                                  1);

	if (ppa_ntfs_try_get_entry_by_utf8_path(candidate_with_root, entry))
		return 1;

	return 0;
}

static int ppa_ntfs_get_entry_by_path(const char *path,
                                      libfsntfs_file_entry_t **entry)
{
	char path_copy[PPA_NTFS_MAX_PATH];
	const char *relative;
	char *cursor;
	libfsntfs_error_t *error = 0;
	libfsntfs_file_entry_t *current = 0;

	if (entry == 0)
		return 0;

	*entry = 0;

	if (!g_ntfs_available || g_ntfs_volume == 0)
		return 0;

	relative = ppa_ntfs_strip_prefix(path);

	if (relative == 0)
		relative = "";

	while (*relative == '/' || *relative == '\\')
		relative++;

	if (strlen(relative) >= sizeof(path_copy)) {
		return 0;
	}

	if (relative[0] == 0) {
		if (libfsntfs_volume_get_root_directory(g_ntfs_volume,
		                                        entry,
		                                        &error) != 1) {
			ppa_ntfs_free_fs_error(&error);
			return 0;
		}

		ppa_ntfs_free_fs_error(&error);
		return 1;
	}

	snprintf(path_copy, sizeof(path_copy), "%s", relative);
	path_copy[sizeof(path_copy) - 1] = 0;

	if (!ppa_ntfs_relative_path_is_safe(path_copy)) {
		return 0;
	}

	/*
	 * Try libfsntfs' full-path resolver before manual child walking.
	 * The manual path is kept as a fallback because older/quirky PSP builds
	 * may still behave better for simple top-level paths.
	 */
	if (ppa_ntfs_get_entry_by_direct_utf8_path(path_copy, entry)) {
		return 1;
	}

	if (libfsntfs_volume_get_root_directory(g_ntfs_volume,
	                                        &current,
	                                        &error) != 1) {
		ppa_ntfs_free_fs_error(&error);
		return 0;
	}

	ppa_ntfs_free_fs_error(&error);

	cursor = path_copy;

	while (cursor != 0 && *cursor != 0) {
		char component[256];
		char *next;
		size_t component_len;
		libfsntfs_file_entry_t *child = 0;

		while (*cursor == '/' || *cursor == '\\')
			cursor++;

		if (*cursor == 0)
			break;

		next = cursor;

		while (*next != 0 && *next != '/' && *next != '\\')
			next++;

		component_len = (size_t)(next - cursor);

		if (component_len >= sizeof(component)) {
			libfsntfs_file_entry_free(&current, &error);
			ppa_ntfs_free_fs_error(&error);
			return 0;
		}

		memcpy(component, cursor, component_len);
		component[component_len] = 0;

		cursor = next;

		if (component[0] == 0 || strcmp(component, ".") == 0)
			continue;

		if (strcmp(component, "..") == 0) {

			libfsntfs_file_entry_free(&current, &error);
			ppa_ntfs_free_fs_error(&error);
			return 0;
		}

		if (!ppa_ntfs_component_is_safe(component)) {

			libfsntfs_file_entry_free(&current, &error);
			ppa_ntfs_free_fs_error(&error);
			return 0;
		}

		if (!ppa_ntfs_find_child_by_name(current, component, &child)) {
			libfsntfs_file_entry_free(&current, &error);
			ppa_ntfs_free_fs_error(&error);

			return 0;
		}

		libfsntfs_file_entry_free(&current, &error);
		ppa_ntfs_free_fs_error(&error);

		current = child;
	}

	*entry = current;

	return 1;
}

static int ppa_ntfs_make_child_relative_path(const char *dir_path,
                                             const char *name,
                                             char *out,
                                             size_t out_size)
{
	const char *relative;
	size_t relative_len;
	size_t name_len;

	if (out == 0 || out_size == 0)
		return 0;

	out[0] = 0;

	if (name == 0 || !ppa_ntfs_component_is_safe(name))
		return 0;

	relative = ppa_ntfs_strip_prefix(dir_path);
	if (relative == 0)
		relative = "";

	while (*relative == '/' || *relative == '\\')
		relative++;

	relative_len = strlen(relative);
	while (relative_len > 0 &&
	       (relative[relative_len - 1] == '/' || relative[relative_len - 1] == '\\')) {
		relative_len--;
	}

	name_len = strlen(name);

	if (relative_len == 0) {
		if (name_len >= out_size)
			return 0;

		memcpy(out, name, name_len + 1);
		return ppa_ntfs_relative_path_is_safe(out);
	}

	if (relative_len + 1 + name_len >= out_size)
		return 0;

	memcpy(out, relative, relative_len);
	out[relative_len] = '/';
	memcpy(out + relative_len + 1, name, name_len + 1);

	return ppa_ntfs_relative_path_is_safe(out);
}

static int ppa_ntfs_get_child_entry_by_utf8_name(libfsntfs_file_entry_t *dir,
                                                 const char *name,
                                                 libfsntfs_file_entry_t **out_child)
{
	libfsntfs_error_t *error = 0;
	libfsntfs_file_entry_t *child = 0;
	size_t name_len;
	int result;

	if (out_child == 0)
		return 0;

	*out_child = 0;

	if (dir == 0 || name == 0 || name[0] == 0)
		return 0;

	if (!ppa_ntfs_component_is_safe(name))
		return 0;

	name_len = strlen(name);

	result = libfsntfs_file_entry_get_sub_file_entry_by_utf8_name(
	             dir,
	             (const uint8_t *)name,
	             name_len,
	             &child,
	             &error);
	ppa_ntfs_free_fs_error(&error);

	if (result == 1 && child != 0) {
		*out_child = child;
		return 1;
	}

	if (child != 0) {
		libfsntfs_file_entry_free(&child, &error);
		ppa_ntfs_free_fs_error(&error);
		child = 0;
	}

	/* Some libyal PSP builds expect the terminating NUL to be included. */
	result = libfsntfs_file_entry_get_sub_file_entry_by_utf8_name(
	             dir,
	             (const uint8_t *)name,
	             name_len + 1,
	             &child,
	             &error);
	ppa_ntfs_free_fs_error(&error);

	if (result == 1 && child != 0) {
		*out_child = child;
		return 1;
	}

	if (child != 0) {
		libfsntfs_file_entry_free(&child, &error);
		ppa_ntfs_free_fs_error(&error);
	}

	return 0;
}

static int ppa_ntfs_get_listing_child_entry(const char *dir_path,
                                            libfsntfs_file_entry_t *dir,
                                            const char *name,
                                            libfsntfs_file_entry_t **out_entry)
{
	char child_relative[PPA_NTFS_MAX_PATH];

	if (out_entry == 0)
		return 0;

	*out_entry = 0;

	if (name == 0 || name[0] == 0)
		return 0;

	/*
	 * Entries returned while walking a parent directory can be lightweight
	 * $I30/$FILE_NAME index entries. They are good enough to obtain the
	 * display name, but on some libfsntfs builds they are not sufficient for
	 * reliable nested-file flags, stream checks, or sizes. Reopen the child
	 * by full path first, and only fall back to parent/name lookup if that
	 * resolver cannot handle the path spelling.
	 */
	if (ppa_ntfs_make_child_relative_path(dir_path,
	                                      name,
	                                      child_relative,
	                                      sizeof(child_relative)) &&
	    ppa_ntfs_get_entry_by_direct_utf8_path(child_relative, out_entry)) {
		return 1;
	}

	return ppa_ntfs_get_child_entry_by_utf8_name(dir, name, out_entry);
}

static void ppa_ntfs_copy_string(char *dst, size_t dst_size, const char *src)
{
	if (dst == 0 || dst_size == 0)
		return;

	if (src == 0) {
		dst[0] = 0;
		return;
	}

	snprintf(dst, dst_size, "%s", src);
	dst[dst_size - 1] = 0;
}

static void ppa_ntfs_set_item(directory_item_struct *item,
                              const char *name,
                              u32 filetype,
                              u64 filesize)
{
	if (item == 0)
		return;

	memset(item, 0, sizeof(*item));

	ppa_ntfs_copy_string(item->longname, sizeof(item->longname), name);
	ppa_ntfs_copy_string(item->shortname, sizeof(item->shortname), name);

	item->compname = item->longname;
	item->filetype = (file_type_enum)filetype;
	item->filesize = filesize;
}

static int ppa_ntfs_append_item(directory_item_struct **items,
                                int *count,
                                int *capacity,
                                const char *name,
                                u32 filetype,
                                u64 filesize)
{
	directory_item_struct *new_items;
	int new_capacity;

	if (items == 0 || count == 0 || capacity == 0)
		return -1;

	if (*count >= PPA_NTFS_MAX_DIR_ENTRIES)
		return 0;

	if (*count >= *capacity) {
		new_capacity = *capacity + PPA_NTFS_DIR_GROW_COUNT;

		if (new_capacity > PPA_NTFS_MAX_DIR_ENTRIES)
			new_capacity = PPA_NTFS_MAX_DIR_ENTRIES;

		new_items = (directory_item_struct *)realloc(*items,
		                                             sizeof(directory_item_struct) *
		                                             (size_t)new_capacity);
		if (new_items == 0)
			return -1;

		memset(&new_items[*capacity],
		       0,
		       sizeof(directory_item_struct) *
		       (size_t)(new_capacity - *capacity));

		*items = new_items;
		*capacity = new_capacity;
	}

	ppa_ntfs_set_item(&(*items)[*count], name, filetype, filesize);
	(*count)++;

	return 1;
}

static int ppa_ntfs_compare_directory_item(const void *a, const void *b)
{
	const directory_item_struct *item1 = (const directory_item_struct *)a;
	const directory_item_struct *item2 = (const directory_item_struct *)b;
	const char *s1;
	const char *s2;
	int item1_is_parent;
	int item2_is_parent;

	if (item1->filetype == FS_DIRECTORY && item2->filetype != FS_DIRECTORY)
		return -1;

	if (item1->filetype != FS_DIRECTORY && item2->filetype == FS_DIRECTORY)
		return 1;

	item1_is_parent = stricmp(item1->longname, "..") == 0;
	item2_is_parent = stricmp(item2->longname, "..") == 0;

	if (item1_is_parent && item2_is_parent)
		return 0;

	if (item1_is_parent)
		return -1;

	if (item2_is_parent)
		return 1;

	s1 = item1->longname;
	s2 = item2->longname;

	while (*s1 != 0 && *s2 != 0) {
		unsigned char c1 = (unsigned char)*s1++;
		unsigned char c2 = (unsigned char)*s2++;

		if (c1 >= 'a' && c1 <= 'z')
			c1 -= 'a' - 'A';

		if (c2 >= 'a' && c2 <= 'z')
			c2 -= 'a' - 'A';

		if (c1 > c2)
			return 1;

		if (c1 < c2)
			return -1;
	}

	if (*s1 != 0)
		return 1;

	if (*s2 != 0)
		return -1;

	return 0;
}

int ppa_ntfs_readdir(const char *path,
                     int show_hidden,
                     int show_unknown,
                     file_type_ext_struct *filter,
                     int names_only,
                     directory_item_struct **out_items)
{
	libfsntfs_error_t *error = 0;
	libfsntfs_file_entry_t *dir = 0;
	libfsntfs_file_entry_t *child = 0;
	libfsntfs_file_entry_t *info_entry = 0;
	int count = 0;
	int capacity = 0;
	int accepted = 0;
	int root_listing;
	int i;
	directory_item_struct *items = 0;

	if (out_items == 0 || ppa_io_cancel_requested())
		return -1;

	root_listing = ppa_ntfs_path_is_volume_root(path);

	if (*out_items != 0) {
		free((void *)(*out_items));
		*out_items = 0;
	}

	if (!ppa_ntfs_get_entry_by_path(path, &dir))
		return -1;

	if (libfsntfs_file_entry_get_number_of_sub_file_entries(dir, &count, &error) != 1) {
		ppa_ntfs_free_fs_error(&error);

		libfsntfs_file_entry_free(&dir, &error);
		ppa_ntfs_free_fs_error(&error);

		return -1;
	}

	ppa_ntfs_free_fs_error(&error);

	if (count < 0)
		count = 0;

	if (count > PPA_NTFS_MAX_DIR_ENTRIES - 1)
		count = PPA_NTFS_MAX_DIR_ENTRIES - 1;

	/*
	 * +1 for ".." entry, matching the rest of PPA's browser behavior.
	 */
	if (ppa_ntfs_append_item(&items, &accepted, &capacity, "..", FS_DIRECTORY, 0) < 0) {
		libfsntfs_file_entry_free(&dir, &error);
		ppa_ntfs_free_fs_error(&error);
		return -1;
	}

	for (i = 0; i < count; i++) {
		char name[256];
		uint32_t flags = 0;
		int flags_valid = 0;
		int is_corrupt = 0;
		int is_dir = 0;
		int child_reopened = 0;
		u32 filetype;
		size64_t size64 = 0;

		if (ppa_io_cancel_requested())
			break;

		child = 0;
		error = 0;

		if (libfsntfs_file_entry_get_sub_file_entry_by_index(dir,
		                                                     i,
		                                                     &child,
		                                                     &error) != 1) {
			ppa_ntfs_free_fs_error(&error);
			continue;
		}

		if (!ppa_ntfs_entry_name(child, name, sizeof(name))) {
			libfsntfs_file_entry_free(&child, &error);
			ppa_ntfs_free_fs_error(&error);
			continue;
		}

		if (ppa_ntfs_name_is_browser_hidden(name)) {
			libfsntfs_file_entry_free(&child, &error);
			ppa_ntfs_free_fs_error(&error);
			continue;
		}

		/*
		 * The volume root uses the original, known-good enumeration path.
		 * In particular, always reopen the child and always run the directory
		 * index fallback even when an attribute-flags call returned a value.
		 * The optimized $I30-only classification remains enabled below root.
		 */
		if (root_listing) {
			info_entry = 0;
			child_reopened = ppa_ntfs_get_listing_child_entry(path,
			                                                  dir,
			                                                  name,
			                                                  &info_entry);
			if (!child_reopened || info_entry == 0) {
				info_entry = child;
				child_reopened = 0;
			}

			flags_valid = ppa_ntfs_get_entry_flags(info_entry, &flags);
			is_dir = ppa_ntfs_entry_is_directory(info_entry,
			                                          flags,
			                                          flags_valid);
		} else {
			/* Use the parent directory's $I30 child entry first. Reopening every
			 * child by full path made listing time depend on large stream metadata
			 * and fragmentation. Only entries whose lightweight record cannot even
			 * establish flags/directory status take the compatibility fallback. */
			info_entry = child;
			flags_valid = ppa_ntfs_get_entry_flags(info_entry, &flags);
			is_dir = flags_valid ?
				((flags & PPA_NTFS_FILE_ATTRIBUTE_DIRECTORY) != 0) :
				ppa_ntfs_entry_is_directory(info_entry, flags, flags_valid);

			if (!flags_valid && !is_dir) {
				libfsntfs_file_entry_t *reopened_entry = 0;
				child_reopened = ppa_ntfs_get_listing_child_entry(path,
				                                                  dir,
				                                                  name,
				                                                  &reopened_entry);
				if (child_reopened && reopened_entry != 0) {
					info_entry = reopened_entry;
					flags = 0;
					flags_valid = ppa_ntfs_get_entry_flags(info_entry, &flags);
					is_dir = flags_valid ?
						((flags & PPA_NTFS_FILE_ATTRIBUTE_DIRECTORY) != 0) :
						ppa_ntfs_entry_is_directory(info_entry,
						                            flags,
						                            flags_valid);
				}
			}
		}

		/* The menu/cache names-only path deliberately avoids opening stream
		 * metadata whose cost can scale with a large or fragmented movie. The
		 * full listing path retains the legacy corruption probe. */
		if (!names_only) {
			error = 0;
			is_corrupt = libfsntfs_file_entry_is_corrupted(info_entry, &error);
			ppa_ntfs_free_fs_error(&error);
		}

		if (is_corrupt == 1) {
			if (child_reopened && info_entry != 0) {
				libfsntfs_file_entry_free(&info_entry, &error);
				ppa_ntfs_free_fs_error(&error);
			}

			libfsntfs_file_entry_free(&child, &error);
			ppa_ntfs_free_fs_error(&error);
			continue;
		}

		if (!show_hidden &&
		    flags_valid &&
		    ((flags & PPA_NTFS_FILE_ATTRIBUTE_HIDDEN) != 0 ||
		     (flags & PPA_NTFS_FILE_ATTRIBUTE_SYSTEM) != 0)) {
			if (child_reopened && info_entry != 0) {
				libfsntfs_file_entry_free(&info_entry, &error);
				ppa_ntfs_free_fs_error(&error);
			}

			libfsntfs_file_entry_free(&child, &error);
			ppa_ntfs_free_fs_error(&error);
			continue;
		}

		/*
		 * Reparse points are not followed. They can represent junctions,
		 * symlinks, or other indirections that are not useful to this
		 * PSP media browser and could create loops.
		 */
		if ((root_listing &&
		     ppa_ntfs_entry_is_reparse_point(info_entry, flags, flags_valid)) ||
		    (!root_listing &&
		     ((flags_valid &&
		       ((flags & PPA_NTFS_FILE_ATTRIBUTE_REPARSE) != 0)) ||
		      (!flags_valid &&
		       ppa_ntfs_entry_is_reparse_point(info_entry,
		                                       flags,
		                                       flags_valid))))) {
			if (child_reopened && info_entry != 0) {
				libfsntfs_file_entry_free(&info_entry, &error);
				ppa_ntfs_free_fs_error(&error);
			}

			libfsntfs_file_entry_free(&child, &error);
			ppa_ntfs_free_fs_error(&error);
			continue;
		}

		filetype = is_dir ? FS_DIRECTORY : ppa_ntfs_filetype_from_name(name);

		if (!ppa_ntfs_filter_accept(filter, name, filetype, show_unknown)) {
			if (child_reopened && info_entry != 0) {
				libfsntfs_file_entry_free(&info_entry, &error);
				ppa_ntfs_free_fs_error(&error);
			}

			libfsntfs_file_entry_free(&child, &error);
			ppa_ntfs_free_fs_error(&error);
			continue;
		}

		/* Preserve full file sizes without making names-only enumeration do
		 * extra NTFS metadata I/O. Browser cache records still contain no size. */
		if (!names_only && !is_dir) {
			int have_size = root_listing ?
				ppa_ntfs_get_listing_size_legacy(info_entry, &size64) :
				ppa_ntfs_get_listing_size_fast(child, &size64);
			if (!have_size)
				size64 = 0;
		}

		ppa_ntfs_free_fs_error(&error);

		if (ppa_ntfs_append_item(&items,
		                         &accepted,
		                         &capacity,
		                         name,
		                         filetype,
		                         (u64)size64) < 0) {
			if (child_reopened && info_entry != 0) {
				libfsntfs_file_entry_free(&info_entry, &error);
				ppa_ntfs_free_fs_error(&error);
			}

			libfsntfs_file_entry_free(&child, &error);
			ppa_ntfs_free_fs_error(&error);

			free(items);

			libfsntfs_file_entry_free(&dir, &error);
			ppa_ntfs_free_fs_error(&error);

			return -1;
		}

		if (child_reopened && info_entry != 0) {
			libfsntfs_file_entry_free(&info_entry, &error);
			ppa_ntfs_free_fs_error(&error);
		}

		info_entry = 0;

		libfsntfs_file_entry_free(&child, &error);
		ppa_ntfs_free_fs_error(&error);
	}

	libfsntfs_file_entry_free(&dir, &error);
	ppa_ntfs_free_fs_error(&error);

	if (ppa_io_cancel_requested()) {
		free(items);
		*out_items = 0;
		return -1;
	}

	if (accepted > 1) {
		qsort(items,
		      (size_t)accepted,
		      sizeof(directory_item_struct),
		      ppa_ntfs_compare_directory_item);

		/*
		 * directory_item_struct contains an internal pointer:
		 *   compname -> longname or shortname
		 *
		 * qsort() swaps the struct bytes directly, so every compname pointer
		 * can end up pointing at another array element's longname after sorting.
		 * That made selecting a visible NTFS directory append an unrelated file
		 * name, e.g. opening "ms1:/test_tracks.mkv/" instead of "ms1:/VIDEO/".
		 *
		 * NTFS path navigation intentionally uses the UTF-8 long name stored in
		 * longname, so rebuild these pointers after qsort before the browser sees
		 * the list.
		 */
		for (i = 0; i < accepted; i++)
			items[i].compname = items[i].longname;
	}

	*out_items = items;
	return accepted;
}

ppa_ntfs_file *ppa_ntfs_open(const char *path)
{
	ppa_ntfs_file *file;
	libfsntfs_file_entry_t *entry = 0;
	libfsntfs_error_t *error = 0;
	size64_t size = 0;
	uint32_t flags = 0;
	int flags_valid = 0;
	int has_stream = 0;

	if (path == 0) {
		return 0;
	}

	if (!ppa_ntfs_get_entry_by_path(path, &entry)) {
		return 0;
	}

	flags_valid = ppa_ntfs_get_entry_flags(entry, &flags);

	if (ppa_ntfs_entry_is_directory(entry, flags, flags_valid)) {

		libfsntfs_file_entry_free(&entry, &error);
		ppa_ntfs_free_fs_error(&error);
		return 0;
	}

	if (ppa_ntfs_entry_is_reparse_point(entry, flags, flags_valid)) {

		libfsntfs_file_entry_free(&entry, &error);
		ppa_ntfs_free_fs_error(&error);
		return 0;
	}

	has_stream = libfsntfs_file_entry_has_default_data_stream(entry, &error);
	ppa_ntfs_free_fs_error(&error);

	if (has_stream != 1) {

		libfsntfs_file_entry_free(&entry, &error);
		ppa_ntfs_free_fs_error(&error);
		return 0;
	}

	error = 0;

	if (libfsntfs_file_entry_get_size(entry, &size, &error) != 1) {

		ppa_ntfs_free_fs_error(&error);

		libfsntfs_file_entry_free(&entry, &error);
		ppa_ntfs_free_fs_error(&error);
		return 0;
	}

	ppa_ntfs_free_fs_error(&error);

	file = (ppa_ntfs_file *)malloc(sizeof(*file));
	if (file == 0) {

		libfsntfs_file_entry_free(&entry, &error);
		ppa_ntfs_free_fs_error(&error);
		return 0;
	}

	memset(file, 0, sizeof(*file));

	file->entry = entry;
	file->size = size;
	file->offset = 0;

	strncpy(file->path, path, sizeof(file->path) - 1);
	file->path[sizeof(file->path) - 1] = 0;

	return file;
}

void ppa_ntfs_close(ppa_ntfs_file *file)
{
	libfsntfs_error_t *error = 0;

	if (file == 0)
		return;

	if (file->entry != 0) {
		libfsntfs_file_entry_free(&file->entry, &error);
		ppa_ntfs_free_fs_error(&error);
	}

	free(file);
}

int64_t ppa_ntfs_seek64(ppa_ntfs_file *file, int64_t position)
{
	if (file == 0)
		return -1;

	if (position < 0)
		position = 0;

	if ((uint64_t)position > (uint64_t)file->size) {
		if ((uint64_t)file->size > (uint64_t)INT64_MAX)
			position = INT64_MAX;
		else
			position = (int64_t)file->size;
	}

	file->offset = (off64_t)position;
	return position;
}

int64_t ppa_ntfs_tell64(ppa_ntfs_file *file)
{
	if (file == 0)
		return -1;

	return (int64_t)file->offset;
}

int64_t ppa_ntfs_size64(ppa_ntfs_file *file)
{
	if (file == 0)
		return -1;

	if ((uint64_t)file->size > (uint64_t)INT64_MAX)
		return INT64_MAX;

	return (int64_t)file->size;
}

int ppa_ntfs_read_at64(ppa_ntfs_file *file,
                       uint64_t position,
                       void *buffer,
                       unsigned int size)
{
	uint8_t *out;
	uint64_t remaining;
	unsigned int total = 0;

	if (file == 0 || file->entry == 0 || buffer == 0)
		return -1;

	if (size == 0)
		return 0;

	if (position >= (uint64_t)file->size)
		return 0;

	remaining = (uint64_t)file->size - position;

	if ((uint64_t)size > remaining)
		size = (unsigned int)remaining;

	if (size > 0x7fffffffU)
		size = 0x7fffffffU;

	out = (uint8_t *)buffer;

	while (total < size) {
		libfsntfs_error_t *error = 0;
		unsigned int chunk = size - total;
		ssize_t result;
		if (ppa_io_cancel_requested())
			return total > 0 ? (int)total : -1;

		if (chunk > PPA_NTFS_FILE_READ_CHUNK_SIZE)
			chunk = PPA_NTFS_FILE_READ_CHUNK_SIZE;

		result = libfsntfs_file_entry_read_buffer_at_offset(
		             file->entry,
		             out + total,
		             chunk,
		             (off64_t)(position + (uint64_t)total),
		             &error);
		ppa_ntfs_free_fs_error(&error);

		if (result < 0) {
			return total > 0 ? (int)total : -1;
		}

		if (result == 0) {
			break;
		}

		if (result > (ssize_t)chunk) {
			return total > 0 ? (int)total : -1;
		}

		total += (unsigned int)result;
	}

	return (int)total;
}

int ppa_ntfs_seek(ppa_ntfs_file *file, int position)
{
	int64_t result = ppa_ntfs_seek64(file, (int64_t)position);
	if (result < 0)
		return -1;
	if (result > 0x7fffffffLL)
		return 0x7fffffff;
	return (int)result;
}

int ppa_ntfs_tell(ppa_ntfs_file *file)
{
	int64_t result = ppa_ntfs_tell64(file);
	if (result < 0)
		return -1;
	if (result > 0x7fffffffLL)
		return 0x7fffffff;
	return (int)result;
}

int ppa_ntfs_size(ppa_ntfs_file *file)
{
	int64_t result = ppa_ntfs_size64(file);
	if (result < 0)
		return -1;
	if (result > 0x7fffffffLL)
		return 0x7fffffff;
	return (int)result;
}

int ppa_ntfs_read_at(ppa_ntfs_file *file,
                     int position,
                     void *buffer,
                     unsigned int size)
{
	if (position < 0)
		return -1;
	return ppa_ntfs_read_at64(file, (uint64_t)(unsigned int)position,
	                          buffer, size);
}

int ppa_ntfs_read(ppa_ntfs_file *file, void *buffer, unsigned int size)
{
	int result;

	if (file == 0)
		return -1;
	if (file->offset < 0)
		return -1;

	result = ppa_ntfs_read_at64(file, (uint64_t)file->offset, buffer, size);
	if (result > 0)
		file->offset += result;

	return result;
}

#else

struct ppa_ntfs_file {
	int unused;
};

int ppa_ntfs_init(void) { return 0; }
void ppa_ntfs_shutdown(void) {}
int ppa_ntfs_available(void) { return 0; }

int ppa_ntfs_readdir(const char *path,
                     int show_hidden,
                     int show_unknown,
                     file_type_ext_struct *filter,
                     int names_only,
                     directory_item_struct **out_items)
{
	(void)path;
	(void)show_hidden;
	(void)show_unknown;
	(void)filter;
	(void)names_only;
	(void)out_items;
	return -1;
}

ppa_ntfs_file *ppa_ntfs_open(const char *path)
{
	(void)path;
	return 0;
}

void ppa_ntfs_close(ppa_ntfs_file *file) { (void)file; }
int64_t ppa_ntfs_seek64(ppa_ntfs_file *file, int64_t position) { (void)file; (void)position; return -1; }
int64_t ppa_ntfs_tell64(ppa_ntfs_file *file) { (void)file; return -1; }
int64_t ppa_ntfs_size64(ppa_ntfs_file *file) { (void)file; return -1; }
int ppa_ntfs_read_at64(ppa_ntfs_file *file, uint64_t position, void *buffer, unsigned int size)
{ (void)file; (void)position; (void)buffer; (void)size; return -1; }
int ppa_ntfs_seek(ppa_ntfs_file *file, int position) { (void)file; (void)position; return -1; }
int ppa_ntfs_tell(ppa_ntfs_file *file) { (void)file; return -1; }
int ppa_ntfs_size(ppa_ntfs_file *file) { (void)file; return -1; }
int ppa_ntfs_read(ppa_ntfs_file *file, void *buffer, unsigned int size)
{
	(void)file;
	(void)buffer;
	(void)size;
	return -1;
}
int ppa_ntfs_read_at(ppa_ntfs_file *file, int position, void *buffer, unsigned int size)
{
	(void)file;
	(void)position;
	(void)buffer;
	(void)size;
	return -1;
}

int ppa_ntfs_path_is_ntfs(const char *path)
{
	return path != 0 &&
	       path[0] == 'm' &&
	       path[1] == 's' &&
	       path[2] == '1' &&
	       path[3] == ':';
}

const char *ppa_ntfs_strip_prefix(const char *path)
{
	if (!ppa_ntfs_path_is_ntfs(path))
		return path;

	path += 4;
	if (*path == '/')
		path++;

	return path;
}

#endif
