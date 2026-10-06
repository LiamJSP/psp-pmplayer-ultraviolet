/*
 *	Copyright (C) 2009 cooleyes
 *	eyes.cooleyes@gmail.com
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU Make; see the file COPYING.
 *  If not, write to the Free Software Foundation, 675 Mass Ave, Cambridge,
 *  MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <pspthreadman.h>

#include "bufferedio.h"

#define IO_CANCEL_SLOT_COUNT 4

typedef struct io_cancel_slot {
	volatile SceUID thread_id;
	io_cancel_check_fn check;
	void *user;
} io_cancel_slot;

static io_cancel_slot g_io_cancel_slots[IO_CANCEL_SLOT_COUNT];
static SceUID g_io_cancel_lock = -1;

static int io_cancel_lock_init(void)
{
	if (g_io_cancel_lock >= 0)
		return 1;
	g_io_cancel_lock = sceKernelCreateSema("bufferedio_cancel", 0, 1, 1, 0);
	return g_io_cancel_lock >= 0;
}

void io_set_cancel_check(io_cancel_check_fn check, void *user)
{
	SceUID thread_id = sceKernelGetThreadId();
	int free_slot = -1;
	int i;
	if (!io_cancel_lock_init() ||
	    sceKernelWaitSema(g_io_cancel_lock, 1, 0) < 0)
		return;
	for (i = 0; i < IO_CANCEL_SLOT_COUNT; ++i) {
		if (g_io_cancel_slots[i].thread_id == thread_id) {
			if (check == 0) {
				g_io_cancel_slots[i].check = 0;
				g_io_cancel_slots[i].user = 0;
				g_io_cancel_slots[i].thread_id = 0;
			}
			else {
				g_io_cancel_slots[i].user = user;
				g_io_cancel_slots[i].check = check;
			}
			sceKernelSignalSema(g_io_cancel_lock, 1);
			return;
		}
		if (free_slot < 0 && g_io_cancel_slots[i].thread_id == 0)
			free_slot = i;
	}
	if (check != 0 && free_slot >= 0) {
		g_io_cancel_slots[free_slot].user = user;
		g_io_cancel_slots[free_slot].thread_id = thread_id;
		g_io_cancel_slots[free_slot].check = check;
	}
	sceKernelSignalSema(g_io_cancel_lock, 1);
}

static int io_cancelled(void)
{
	SceUID thread_id = sceKernelGetThreadId();
	int i;
	for (i = 0; i < IO_CANCEL_SLOT_COUNT; ++i) {
		io_cancel_check_fn check;
		if (g_io_cancel_slots[i].thread_id != thread_id)
			continue;
		check = g_io_cancel_slots[i].check;
		return check != 0 && check(g_io_cancel_slots[i].user);
	}
	return 0;
}

/*
 * libbufferedio.psp lives outside trunk/ppa, but final PPA links
 * common/ppa_vfs.o. Declare only the small VFS surface needed here instead of
 * including ppa_vfs.h directly.
 */
typedef struct ppa_vfs_file ppa_vfs_file;

extern int ppa_vfs_is_ntfs_path(const char *path);
extern ppa_vfs_file *ppa_vfs_open(const char *path);
extern void ppa_vfs_close(ppa_vfs_file *file);
extern int64_t ppa_vfs_size64(ppa_vfs_file *file);
extern int ppa_vfs_read_at64(ppa_vfs_file *file,
                           uint64_t position,
                           void *buffer,
                           unsigned int size);

static int io_is_vfs_path(const char *filename)
{
	return filename != 0 && ppa_vfs_is_ntfs_path(filename);
}

static int64_t io_clamp_position(buffered_io_t *io, int64_t position)
{
	if (io == 0)
		return 0;

	if (position < 0)
		return 0;

	if (position > io->length)
		return io->length;

	return position;
}

static int64_t io_align64_down(int64_t position)
{
	return position & ~63LL;
}

static int64_t io_backend_length(buffered_io_t *io)
{
	if (io == 0)
		return -1;

	if (io->mode == 1) {
		if (io->vfs_handle == 0)
			return -1;

		return ppa_vfs_size64((ppa_vfs_file *)io->vfs_handle);
	}

	if (io->handle < 0)
		return -1;

	return sceIoLseek(io->handle, 0, PSP_SEEK_END);
}

static int32_t io_backend_read_at(buffered_io_t *io,
                                  int64_t position,
                                  uint8_t *buffer,
                                  uint32_t size)
{
	int result;

	if (io == 0 || buffer == 0)
		return -1;

	if (size == 0)
		return 0;

	if (position < 0)
		return -1;

	if (io->mode == 1) {
		if (io->vfs_handle == 0)
			return -1;

		return ppa_vfs_read_at64((ppa_vfs_file *)io->vfs_handle,
		                       position,
		                       buffer,
		                       size);
	}

	if (io->handle < 0)
		return -1;

	if (sceIoLseek(io->handle, position, PSP_SEEK_SET) < 0)
		return -1;

	result = sceIoRead(io->handle, buffer, size);
	if (result < 0)
		return -1;

	return result;
}

static void io_reset_state(buffered_io_t *io)
{
	if (io == 0)
		return;

	io->mode = 0;
	io->handle = -1;
	io->vfs_handle = 0;
	io->length = 0;
	io->cache_first_position = 0;
	io->cache_last_position = 0;
	io->current_position = 0;
}

int32_t io_open64(const char* filename, void* handle)
{
	buffered_io_t* io = (buffered_io_t*)handle;

	if (io == 0 || filename == 0)
		return -1;

	memset(io, 0, sizeof(*io));
	io_reset_state(io);
	if (io_cancelled())
		return -1;

	if (io_is_vfs_path(filename)) {
		io->mode = 1;
		io->vfs_handle = ppa_vfs_open(filename);

		if (io->vfs_handle == 0) {
			io_reset_state(io);
			return -1;
		}

		io->length = ppa_vfs_size64((ppa_vfs_file *)io->vfs_handle);

		if (io->length < 0) {
			io_close(handle);
			return -1;
		}

	}
	else {
		io->mode = 0;
		io->handle = sceIoOpen(filename, PSP_O_RDONLY, 0777);

		if (io->handle < 0) {
			io_reset_state(io);
			return -1;
		}

		io->length = io_backend_length(io);

		if (io->length < 0) {
			io_close(handle);
			return -1;
		}
	}

	if (io_set_position64(handle, 0) < 0) {
		io_close(handle);
		return -1;
	}

	return 0;
}

void io_close(void* handle)
{
	buffered_io_t* io = (buffered_io_t*)handle;

	if (io == 0)
		return;

	if (io->mode == 1) {
		if (io->vfs_handle != 0) {
			ppa_vfs_close((ppa_vfs_file *)io->vfs_handle);
			io->vfs_handle = 0;
		}
	}
	else {
		if (io->handle >= 0) {
			sceIoClose(io->handle);
			io->handle = -1;
		}
	}

	io_reset_state(io);
}

int64_t io_get_length64(void* handle)
{
	buffered_io_t* io = (buffered_io_t*)handle;

	if (io == 0)
		return -1;

	return io->length;
}

int64_t io_set_position64(void* handle, int64_t position)
{
	buffered_io_t* io = (buffered_io_t*)handle;
	int64_t clamped_position;
	int64_t new_cache_first;
	int64_t new_cache_last;
	int32_t read_size;
	int32_t result;

	if (io == 0)
		return -1;

	if (io_cancelled()) {
		io->current_position = io->length;
		return -1;
	}

	if (io->length < 0)
		return -1;

	clamped_position = io_clamp_position(io, position);

	if (clamped_position >= io->cache_first_position &&
	    clamped_position < io->cache_last_position) {
		io->current_position = clamped_position;
		return clamped_position;
	}

	if (clamped_position >= io->length)
		new_cache_first = io->length;
	else
		new_cache_first = io_align64_down(clamped_position);

	new_cache_last = new_cache_first;

	if (new_cache_first < io->length) {
		int64_t available_to_eof = io->length - new_cache_first;

		if (available_to_eof > CACHE_BUFFER_SIZE)
			available_to_eof = CACHE_BUFFER_SIZE;

		if (available_to_eof < 0)
			return -1;

		new_cache_last = new_cache_first + available_to_eof;
	}

	read_size = new_cache_last - new_cache_first;

	if (read_size > 0) {
		result = io_backend_read_at(io,
		                            new_cache_first,
		                            io->cache_buffer,
		                            (uint32_t)read_size);

		if (result < 0)
			return -1;

		if (result < read_size)
			new_cache_last = new_cache_first + result;

		if (clamped_position < io->length && clamped_position >= new_cache_last)
			return -1;
	}

	io->cache_first_position = new_cache_first;
	io->cache_last_position = new_cache_last;
	io->current_position = clamped_position;

	return io->current_position;
}

int64_t io_get_position64(void* handle)
{
	buffered_io_t* io = (buffered_io_t*)handle;

	if (io == 0)
		return -1;

	return io->current_position;
}

uint32_t io_read_data(void* handle, uint8_t* data, const uint32_t size)
{
	buffered_io_t* io = (buffered_io_t*)handle;
	uint32_t copied = 0;

	if (io == 0 || data == 0)
		return 0;

	if (size == 0)
		return 0;

	if (io_cancelled()) {
		io->current_position = io->length;
		return 0;
	}

	while (copied < size) {
		if (io_cancelled()) {
			io->current_position = io->length;
			break;
		}
		uint32_t available;
		uint32_t chunk;

		if (io->current_position >= io->length)
			break;

		if (!(io->current_position >= io->cache_first_position &&
		      io->current_position < io->cache_last_position)) {
			if (io_set_position64(handle, io->current_position) < 0)
				break;
		}

		if (io->current_position < io->cache_first_position ||
		    io->current_position >= io->cache_last_position)
			break;

		available = (uint32_t)(io->cache_last_position - io->current_position);

		if (available == 0)
			break;

		chunk = size - copied;

		if (chunk > available)
			chunk = available;

		memcpy(data + copied,
		       io->cache_buffer + (io->current_position - io->cache_first_position),
		       chunk);

		io->current_position += chunk;
		copied += chunk;
	}

	return copied;
}

/* Preserve callers that explicitly require the old signed-32 contract. */
int32_t io_open(const char *filename, void *handle)
{
    int32_t result = io_open64(filename, handle);
    if (result == 0 && io_get_length64(handle) > INT32_MAX) {
        io_close(handle);
        return -1;
    }
    return result;
}
int32_t io_get_position(void *handle)
{
    int64_t pos = io_get_position64(handle);
    return pos < 0 || pos > INT32_MAX ? -1 : (int32_t)pos;
}
int32_t io_get_length(void *handle)
{
    int64_t length = io_get_length64(handle);
    return length < 0 || length > INT32_MAX ? -1 : (int32_t)length;
}
int32_t io_set_position(void *handle, const int32_t position)
{
    int64_t pos = io_set_position64(handle, position);
    return pos < 0 || pos > INT32_MAX ? -1 : (int32_t)pos;
}

uint64_t io_read_be64(void* handle)
{
	uint8_t data[8];
	uint64_t result = 0;
	int i;

	if (io_read_data(handle, data, 8) != 8)
		return 0;

	for (i = 0; i < 8; i++) {
		result |= ((uint64_t)data[i]) << ((7 - i) * 8);
	}

	return result;
}

uint32_t io_read_be32(void* handle)
{
	uint8_t data[4];
	uint32_t result = 0;
	uint32_t a, b, c, d;

	if (io_read_data(handle, data, 4) != 4)
		return 0;

	a = (uint8_t)data[0];
	b = (uint8_t)data[1];
	c = (uint8_t)data[2];
	d = (uint8_t)data[3];

	result = (a << 24) | (b << 16) | (c << 8) | d;

	return result;
}

uint32_t io_read_be24(void* handle)
{
	uint8_t data[3];
	uint32_t result = 0;
	uint32_t a, b, c;

	if (io_read_data(handle, data, 3) != 3)
		return 0;

	a = (uint8_t)data[0];
	b = (uint8_t)data[1];
	c = (uint8_t)data[2];

	result = (a << 16) | (b << 8) | c;

	return result;
}

uint16_t io_read_be16(void* handle)
{
	uint8_t data[2];
	uint16_t result = 0;
	uint16_t a, b;

	if (io_read_data(handle, data, 2) != 2)
		return 0;

	a = (uint8_t)data[0];
	b = (uint8_t)data[1];

	result = (a << 8) | b;

	return result;
}

uint8_t io_read_8(void* handle)
{
	uint8_t result = 0;

	io_read_data(handle, &result, 1);

	return result;
}
