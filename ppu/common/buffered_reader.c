
#include "buffered_reader.h"
#include "mem64.h"
#include "ppa_hardware_profile.h"
#include "ppa_vfs.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>

#define PPA_BUFFERED_READER_WINDOW_COUNT        3U
#define PPA_BUFFERED_READER_DEFAULT_MARGIN_US   500000ULL

struct PpaReadWindowInternal {
	struct PpaReadWindow public_window;
	struct PpaIoRequest request;
};

struct buffered_reader_struct {
	ppa_vfs_file *handle;
	struct PpaIoPump *pump;
	int owns_pump;
	uint64_t length;
	uint32_t buffer_size;
	int32_t seek_mode;
	uint32_t priority;
	uint32_t stream_id;
	uint32_t generation;
	uint64_t current_position;
	int current_window;
	uint64_t media_deadline_us;
	uint64_t not_before_us;
	struct PpaReadWindowInternal windows[PPA_BUFFERED_READER_WINDOW_COUNT];
	char path[1024];
};

static uint64_t buffered_reader_now(void)
{
	return (uint64_t)sceKernelGetSystemTimeWide();
}

static uint64_t buffered_reader_clamp_position(buffered_reader_t *reader,
                                               uint64_t position)
{
	if (reader == 0)
		return 0;
	if (position > reader->length)
		return reader->length;
	return position;
}

static uint64_t buffered_reader_align64_down(uint64_t position)
{
	return position & ~63ULL;
}

static int buffered_reader_has_borrows(const buffered_reader_t *reader)
{
	uint32_t i;

	if (reader == 0)
		return 0;
	for (i = 0U; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i) {
		if (reader->windows[i].public_window.references != 0U)
			return 1;
	}
	return 0;
}

static uint32_t buffered_reader_window_bytes(buffered_reader_t *reader,
                                             uint64_t offset)
{
	uint64_t remaining;

	if (reader == 0 || offset >= reader->length)
		return 0;
	remaining = reader->length - offset;
	if (remaining > reader->buffer_size)
		remaining = reader->buffer_size;
	return (uint32_t)remaining;
}

static int buffered_reader_target_size(const char *path, int32_t minimum)
{
	int32_t target = minimum;

	if (!ppa_hardware_has_64mb_ram())
		return minimum;

	if (minimum >= 96 * 1024)
		target = 512 * 1024;
	else if (minimum >= 64 * 1024)
		target = 384 * 1024;
	else
		target = 256 * 1024;

	(void)path;
	return target;
}

static int buffered_reader_allocate_windows(buffered_reader_t *reader,
                                             int32_t minimum)
{
	int32_t candidate;
	uint32_t i;

	if (reader == 0)
		return 0;

	candidate = buffered_reader_target_size(reader->path, minimum);
	for (;;) {
		int complete = 1;
		reader->buffer_size = (uint32_t)candidate;
		for (i = 0; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i) {
			struct PpaReadWindow *window =
				&reader->windows[i].public_window;
			window->data = (uint8_t *)malloc_64((unsigned int)candidate);
			window->capacity = (uint32_t)candidate;
			window->state = PPA_WINDOW_EMPTY;
			if (window->data == 0)
				complete = 0;
		}
		if (complete)
			return 1;

		for (i = 0; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i) {
			struct PpaReadWindow *window =
				&reader->windows[i].public_window;
			if (window->data != 0)
				free_64(window->data);
			memset(window, 0, sizeof(*window));
		}

		if (candidate == minimum)
			return 0;
		candidate /= 2;
		if (candidate < minimum)
			candidate = minimum;
	}
}

static void buffered_reader_cancel_window(buffered_reader_t *reader,
                                          struct PpaReadWindowInternal *internal)
{
	struct PpaReadWindow *window;

	if (reader == 0 || internal == 0)
		return;
	window = &internal->public_window;
	if (window->references != 0U) {
		return;
	}

	if (window->state == PPA_WINDOW_FILLING) {
		ppa_io_pump_cancel(reader->pump, &internal->request, 1);
	}
	window->valid_bytes = 0;
	window->references = 0;
	window->state = PPA_WINDOW_CANCELLED;
}

static int buffered_reader_finish_window(buffered_reader_t *reader,
                                         int index)
{
	struct PpaReadWindowInternal *internal;
	struct PpaReadWindow *window;
	int result;

	if (reader == 0 || index < 0 ||
	    index >= (int)PPA_BUFFERED_READER_WINDOW_COUNT)
		return 0;
	internal = &reader->windows[index];
	window = &internal->public_window;
	if (window->state == PPA_WINDOW_READY ||
	    window->state == PPA_WINDOW_IN_USE)
		return 1;
	if (window->state != PPA_WINDOW_FILLING)
		return 0;

	result = ppa_io_request_wait(&internal->request);

	if (window->generation != reader->generation ||
	    internal->request.generation != reader->generation) {
		window->state = PPA_WINDOW_CANCELLED;
		window->valid_bytes = 0;
		return 0;
	}

	if (result != (int)internal->request.bytes ||
	    internal->request.state != PPA_IO_REQUEST_COMPLETE) {
		window->state = internal->request.state == PPA_IO_REQUEST_CANCELLED ?
		                PPA_WINDOW_CANCELLED : PPA_WINDOW_ERROR;
		window->valid_bytes = 0;
		return 0;
	}

	window->valid_bytes = (uint32_t)result;
	window->state = PPA_WINDOW_READY;
	return 1;
}

static int buffered_reader_submit_window(buffered_reader_t *reader,
                                         int index,
                                         uint64_t offset,
                                         int required)
{
	struct PpaReadWindowInternal *internal;
	struct PpaReadWindow *window;
	uint32_t bytes;
	uint64_t now;
	uint64_t deadline;
	uint64_t not_before;
	uint32_t flags;

	if (reader == 0 || index < 0 ||
	    index >= (int)PPA_BUFFERED_READER_WINDOW_COUNT)
		return 0;
	internal = &reader->windows[index];
	window = &internal->public_window;
	if (window->references != 0U)
		return 0;

	if (window->state == PPA_WINDOW_FILLING)
		buffered_reader_cancel_window(reader, internal);

	window->file_offset = offset;
	window->valid_bytes = 0;
	window->generation = reader->generation;
	window->references = 0;
	bytes = buffered_reader_window_bytes(reader, offset);
	if (bytes == 0U) {
		window->state = PPA_WINDOW_READY;
		return 1;
	}

	now = buffered_reader_now();
	deadline = required ? now : reader->media_deadline_us;
	not_before = required ? now : reader->not_before_us;
	if (deadline == 0)
		deadline = now + PPA_BUFFERED_READER_DEFAULT_MARGIN_US;
	if (not_before == 0)
		not_before = now;

	flags = PPA_IO_REQUEST_PREFER_NATIVE_ASYNC;
	if (required)
		flags |= PPA_IO_REQUEST_REQUIRED;

	window->state = PPA_WINDOW_FILLING;
	if (!ppa_io_pump_submit(reader->pump,
	                        &internal->request,
	                        reader->handle,
	                        offset,
	                        window->data,
	                        bytes,
	                        deadline,
	                        not_before,
	                        reader->stream_id,
	                        reader->generation,
	                        reader,
	                        flags)) {
		window->state = PPA_WINDOW_ERROR;
		return 0;
	}

	if (!required)
		return 1;
	return buffered_reader_finish_window(reader, index);
}

static int buffered_reader_find_window(buffered_reader_t *reader,
                                       uint64_t position)
{
	uint32_t i;

	if (reader == 0)
		return -1;
	for (i = 0; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i) {
		struct PpaReadWindowInternal *internal = &reader->windows[i];
		struct PpaReadWindow *window = &internal->public_window;
		uint32_t span;

		if (window->generation != reader->generation ||
		    window->state == PPA_WINDOW_EMPTY ||
		    window->state == PPA_WINDOW_ERROR ||
		    window->state == PPA_WINDOW_CANCELLED)
			continue;
		span = window->state == PPA_WINDOW_FILLING ?
		       internal->request.bytes : window->valid_bytes;
		if (position >= window->file_offset &&
		    position < window->file_offset + (uint64_t)span)
			return (int)i;
	}
	return -1;
}

static int buffered_reader_find_offset(buffered_reader_t *reader,
                                       uint64_t offset)
{
	uint32_t i;

	for (i = 0; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i) {
		struct PpaReadWindow *window =
			&reader->windows[i].public_window;
		if (window->generation == reader->generation &&
		    window->file_offset == offset &&
		    window->state != PPA_WINDOW_EMPTY &&
		    window->state != PPA_WINDOW_ERROR &&
		    window->state != PPA_WINDOW_CANCELLED)
			return (int)i;
	}
	return -1;
}

static int buffered_reader_find_reusable(buffered_reader_t *reader,
                                         uint64_t current_offset)
{
	uint32_t i;

	for (i = 0; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i) {
		struct PpaReadWindowInternal *internal = &reader->windows[i];
		struct PpaReadWindow *window = &internal->public_window;
		if ((int)i == reader->current_window ||
		    window->references != 0U)
			continue;

		/* A completed speculative window is published lazily because no parser
		 * thread waits for it. Reap its terminal request before deciding whether
		 * the slot can rotate; otherwise an unused seek-behind window could remain
		 * labelled FILLING for the rest of the session. */
		if (window->state == PPA_WINDOW_FILLING &&
		    (internal->request.state == PPA_IO_REQUEST_COMPLETE ||
		     internal->request.state == PPA_IO_REQUEST_ERROR ||
		     internal->request.state == PPA_IO_REQUEST_CANCELLED))
			(void)buffered_reader_finish_window(reader, (int)i);

		if (window->state == PPA_WINDOW_EMPTY ||
		    window->state == PPA_WINDOW_ERROR ||
		    window->state == PPA_WINDOW_CANCELLED ||
		    (window->state == PPA_WINDOW_READY &&
		     window->file_offset < current_offset))
			return (int)i;
	}
	return -1;
}

static void buffered_reader_ensure_lookahead(buffered_reader_t *reader)
{
	struct PpaReadWindow *current;
	uint64_t desired;
	uint32_t ahead;

	if (reader == 0 || reader->current_window < 0 ||
	    reader->current_window >= (int)PPA_BUFFERED_READER_WINDOW_COUNT)
		return;
	current = &reader->windows[reader->current_window].public_window;
	if (current->generation != reader->generation)
		return;

	for (ahead = 1; ahead <= 2; ++ahead) {
		int reusable;
		desired = current->file_offset +
		          (uint64_t)reader->buffer_size * ahead;
		if (desired >= reader->length)
			continue;
		if (buffered_reader_find_offset(reader, desired) >= 0)
			continue;
		reusable = buffered_reader_find_reusable(reader,
		                                         current->file_offset);
		if (reusable < 0)
			continue;
		(void)buffered_reader_submit_window(reader, reusable, desired, 0);
	}
}

static int buffered_reader_select_window(buffered_reader_t *reader,
                                         int index)
{
	struct PpaReadWindow *window;

	if (reader == 0 || index < 0 ||
	    index >= (int)PPA_BUFFERED_READER_WINDOW_COUNT)
		return 0;
	window = &reader->windows[index].public_window;
	if (window->state == PPA_WINDOW_FILLING) {
		uint64_t now = buffered_reader_now();
		/* A speculative refill becomes required the instant the parser reaches
		 * it; override any healthy-queue deferral before blocking. */
		ppa_io_pump_update_schedule(reader->pump,
		                            &reader->windows[index].request,
		                            now, now);
		if (!buffered_reader_finish_window(reader, index))
			return 0;
	}
	if (window->state != PPA_WINDOW_READY &&
	    window->state != PPA_WINDOW_IN_USE)
		return 0;

	if (reader->current_window >= 0 && reader->current_window != index) {
		struct PpaReadWindow *old =
			&reader->windows[reader->current_window].public_window;
		if (old->state == PPA_WINDOW_IN_USE)
			old->state = PPA_WINDOW_READY;
	}
	reader->current_window = index;
	window->state = PPA_WINDOW_IN_USE;
	buffered_reader_ensure_lookahead(reader);
	return 1;
}

static int buffered_reader_reset_at(buffered_reader_t *reader,
                                    uint64_t position)
{
	uint64_t current_offset;
	uint64_t base;
	int required_index = 0;
	uint32_t i;

	if (reader == 0)
		return 0;
	if (buffered_reader_has_borrows(reader)) {
		return 0;
	}
	position = buffered_reader_clamp_position(reader, position);
	reader->generation++;
	if (reader->generation == 0U)
		reader->generation = 1U;

	for (i = 0; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i) {
		buffered_reader_cancel_window(reader, &reader->windows[i]);
		reader->windows[i].public_window.state = PPA_WINDOW_EMPTY;
	}

	current_offset = buffered_reader_align64_down(position);
	base = current_offset;
	if (reader->seek_mode != 0 && current_offset >= reader->buffer_size) {
		base = current_offset - reader->buffer_size;
		required_index = 1;
	}

	reader->current_window = -1;
	reader->current_position = position;
	if (position == reader->length)
		return 1;

	if (!buffered_reader_submit_window(reader, required_index,
	                                  current_offset, 1))
		return 0;
	reader->current_window = required_index;
	reader->windows[required_index].public_window.state = PPA_WINDOW_IN_USE;

	/* Only the required window participates in startup latency. The remaining
	 * windows are submitted after it is published and fill on the pump. */
	for (i = 0; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i) {
		uint64_t offset = base + (uint64_t)reader->buffer_size * i;
		if ((int)i == required_index || offset >= reader->length)
			continue;
		(void)buffered_reader_submit_window(reader, (int)i, offset, 0);
	}
	buffered_reader_ensure_lookahead(reader);
	return 1;
}

void buffered_reader_close(buffered_reader_t *reader)
{
	uint32_t i;

	if (reader == 0)
		return;
	if (buffered_reader_has_borrows(reader)) {
		return;
	}

	reader->generation++;
	for (i = 0; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i)
		buffered_reader_cancel_window(reader, &reader->windows[i]);

	for (i = 0; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i)
		ppa_io_request_destroy(&reader->windows[i].request);

	if (reader->owns_pump && reader->pump != 0) {
		ppa_io_pump_destroy(reader->pump);
		reader->pump = 0;
	}
	if (reader->handle != 0) {
		ppa_vfs_close(reader->handle);
		reader->handle = 0;
	}

	for (i = 0; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i) {
		if (reader->windows[i].public_window.data != 0)
			free_64(reader->windows[i].public_window.data);
	}

	free(reader);
}

buffered_reader_t *buffered_reader_open_with_pump_at(
                                        const char *path,
                                        int32_t buffer_size,
                                        int32_t seek_mode,
                                        uint32_t priority,
                                        struct PpaIoPump *pump,
                                        uint32_t stream_id,
                                        uint64_t initial_position)
{
	buffered_reader_t *reader;
	uint32_t i;
	uint64_t now;

	if (path == 0 || buffer_size <= 0)
		return 0;

	reader = (buffered_reader_t *)malloc(sizeof(*reader));
	if (reader == 0)
		return 0;
	memset(reader, 0, sizeof(*reader));
	for (i = 0; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i)
		reader->windows[i].request.completion_sema = -1;
	reader->current_window = -1;
	reader->seek_mode = seek_mode;
	reader->priority = priority;
	reader->stream_id = stream_id;
	reader->generation = 1U;
	strncpy(reader->path, path, sizeof(reader->path) - 1U);
	reader->path[sizeof(reader->path) - 1U] = 0;

	if (!buffered_reader_allocate_windows(reader, buffer_size)) {
		buffered_reader_close(reader);
		return 0;
	}

	for (i = 0; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i) {
		char name[32];
		snprintf(name, sizeof(name), "ppa_br_%lu_%lu",
		         (unsigned long)stream_id, (unsigned long)i);
		if (!ppa_io_request_init(&reader->windows[i].request, name)) {
			buffered_reader_close(reader);
			return 0;
		}
	}

	reader->handle = ppa_vfs_open(path);
	if (reader->handle == 0) {
		buffered_reader_close(reader);
		return 0;
	}
	{
		int64_t length64 = ppa_vfs_size64(reader->handle);
		if (length64 < 0) {
			buffered_reader_close(reader);
			return 0;
		}
		reader->length = (uint64_t)length64;
	}

	if (pump != 0) {
		reader->pump = pump;
		reader->owns_pump = 0;
	}
	else {
		reader->pump = ppa_io_pump_create();
		reader->owns_pump = 1;
		if (reader->pump == 0) {
			buffered_reader_close(reader);
			return 0;
		}
	}

	now = buffered_reader_now();
	reader->media_deadline_us = now + PPA_BUFFERED_READER_DEFAULT_MARGIN_US;
	reader->not_before_us = now;
	if (!buffered_reader_reset_at(reader, initial_position)) {
		buffered_reader_close(reader);
		return 0;
	}

	return reader;
}

buffered_reader_t *buffered_reader_open(const char *path,
                                        int32_t buffer_size,
                                        int32_t seek_mode,
                                        uint32_t priority)
{
	return buffered_reader_open_with_pump_at(path, buffer_size, seek_mode,
	                                         priority, 0, 0U, 0U);
}

int64_t buffered_reader_length64(buffered_reader_t *reader)
{
	if (reader == 0 || reader->length > (uint64_t)INT64_MAX)
		return reader == 0 ? 0 : INT64_MAX;
	return (int64_t)reader->length;
}

int64_t buffered_reader_seek64(buffered_reader_t *reader, uint64_t position)
{
	int index;

	if (reader == 0)
		return -1;
	if (buffered_reader_has_borrows(reader)) {
		return -1;
	}
	position = buffered_reader_clamp_position(reader, position);
	if (position == reader->length) {
		/* EOF is a real discontinuity: cancel every speculative fill before a
		 * caller can close, reuse, or seek the reader again. */
		if (!buffered_reader_reset_at(reader, position))
			return -1;
		return (int64_t)position;
	}

	index = buffered_reader_find_window(reader, position);
	if (index >= 0 && buffered_reader_select_window(reader, index)) {
		reader->current_position = position;
		return (int64_t)position;
	}

	if (!buffered_reader_reset_at(reader, position))
		return -1;
	return (int64_t)reader->current_position;
}

int64_t buffered_reader_position64(buffered_reader_t *reader)
{
	if (reader == 0 || reader->current_position > (uint64_t)INT64_MAX)
		return reader == 0 ? 0 : INT64_MAX;
	return (int64_t)reader->current_position;
}

uint32_t buffered_reader_read(buffered_reader_t *reader,
                              void *buffer,
                              uint32_t size)
{
	uint8_t *out = (uint8_t *)buffer;
	uint32_t total = 0;

	if (reader == 0 || buffer == 0)
		return 0;

	while (size > 0U && reader->current_position < reader->length) {
		struct PpaReadWindow *window;
		uint64_t local;
		uint32_t available;
		uint32_t chunk;
		int index = buffered_reader_find_window(reader,
		                                        reader->current_position);

		if (index < 0 || !buffered_reader_select_window(reader, index)) {
			if (buffered_reader_seek64(reader,
			                           reader->current_position) < 0)
				break;
			index = reader->current_window;
		}
		if (index < 0)
			break;

		window = &reader->windows[index].public_window;
		local = reader->current_position - window->file_offset;
		if (local >= window->valid_bytes) {
			if (buffered_reader_seek64(reader,
			                           reader->current_position) < 0)
				break;
			continue;
		}

		available = window->valid_bytes - (uint32_t)local;
		chunk = size < available ? size : available;
		memcpy(out + total, window->data + (uint32_t)local, chunk);
		reader->current_position += chunk;
		total += chunk;
		size -= chunk;

		if (reader->current_position >=
		    window->file_offset + window->capacity / 2U)
			buffered_reader_ensure_lookahead(reader);
	}

	return total;
}

int buffered_reader_read_exact(buffered_reader_t *reader,
                               void *buffer,
                               uint32_t size)
{
	return buffered_reader_read(reader, buffer, size) == size;
}

int buffered_reader_borrow_current(buffered_reader_t *reader,
                                   uint32_t size,
                                   uint32_t alignment,
                                   uint32_t readable_tail,
                                   struct PpaBufferedReaderSpan *span)
{
	struct PpaReadWindow *window;
	uint64_t local;
	uint64_t required;
	const uint8_t *data;
	int index;

	if (span != 0)
		memset(span, 0, sizeof(*span));
	if (reader == 0 || span == 0 || size == 0U)
		return 0;
	if (alignment == 0U)
		alignment = 1U;
	if ((alignment & (alignment - 1U)) != 0U) {
		return 0;
	}
	required = (uint64_t)size + readable_tail;
	if (required > UINT_MAX ||
	    reader->current_position > reader->length ||
	    required > reader->length - reader->current_position) {
		return 0;
	}

	index = buffered_reader_find_window(reader, reader->current_position);
	if (index < 0 || !buffered_reader_select_window(reader, index)) {
		return 0;
	}
	window = &reader->windows[index].public_window;
	local = reader->current_position - window->file_offset;
	if (local > window->valid_bytes ||
	    required > (uint64_t)window->valid_bytes - local) {
		return 0;
	}
	data = window->data + (uint32_t)local;
	if (((uintptr_t)data & (uintptr_t)(alignment - 1U)) != 0U ||
	    window->references == UINT_MAX) {
		return 0;
	}

	++window->references;
	span->reader = reader;
	span->data = data;
	span->size = size;
	span->generation = reader->generation;
	span->window_index = index;
	span->active = 1U;
	reader->current_position += size;
	if (reader->current_position >=
	    window->file_offset + window->capacity / 2U)
		buffered_reader_ensure_lookahead(reader);
	return 1;
}

void buffered_reader_release_span(struct PpaBufferedReaderSpan *span)
{
	buffered_reader_t *reader;
	struct PpaReadWindow *window;

	if (span == 0 || !span->active || span->reader == 0)
		return;
	reader = span->reader;
	if (span->window_index >= 0 &&
	    span->window_index < (int32_t)PPA_BUFFERED_READER_WINDOW_COUNT) {
		window = &reader->windows[span->window_index].public_window;
		if (window->generation == span->generation &&
		    window->references != 0U)
			--window->references;
		else
			;
	}
	memset(span, 0, sizeof(*span));
}

void buffered_reader_set_media_schedule(buffered_reader_t *reader,
                                        uint64_t media_deadline_us,
                                        uint64_t not_before_us)
{
	uint32_t i;

	if (reader == 0)
		return;
	reader->media_deadline_us = media_deadline_us;
	reader->not_before_us = not_before_us;
	for (i = 0; i < PPA_BUFFERED_READER_WINDOW_COUNT; ++i) {
		struct PpaReadWindowInternal *internal = &reader->windows[i];
		if (internal->public_window.state == PPA_WINDOW_FILLING &&
		    (internal->request.flags & PPA_IO_REQUEST_REQUIRED) == 0U) {
			ppa_io_pump_update_schedule(reader->pump, &internal->request,
			                            media_deadline_us, not_before_us);
		}
	}
}

void buffered_reader_set_queue_watermarks(buffered_reader_t *reader,
                                          uint32_t queued_duration_ms,
                                          uint32_t low_ms,
                                          uint32_t target_ms,
                                          uint32_t queued_bytes,
                                          uint32_t byte_ceiling)
{
	uint64_t now;
	uint64_t not_before;
	uint64_t deadline;
	uint32_t margin_ms;
	int healthy;

	if (reader == 0)
		return;
	if (target_ms < low_ms)
		target_ms = low_ms;

	now = buffered_reader_now();
	healthy = queued_duration_ms >= target_ms;
	if (byte_ceiling != 0U && queued_bytes >= byte_ceiling)
		healthy = 1;

	/* The deadline is when queued media reaches the low watermark, not when the
	 * entire queue expires. Healthy queues defer the refill until 150 ms before
	 * that edge, preserving large bursts while giving the device a bounded lead.
	 * A queue already at/below low is always urgent, regardless of byte density. */
	margin_ms = queued_duration_ms > low_ms ?
	            queued_duration_ms - low_ms : 0U;
	deadline = now + (uint64_t)margin_ms * 1000ULL;
	if (margin_ms == 0U) {
		not_before = now;
	}
	else if (healthy && margin_ms > 150U) {
		not_before = now + (uint64_t)(margin_ms - 150U) * 1000ULL;
	}
	else {
		not_before = now;
	}
	buffered_reader_set_media_schedule(reader, deadline, not_before);
}

uint32_t buffered_reader_generation(buffered_reader_t *reader)
{
	return reader != 0 ? reader->generation : 0U;
}

uint32_t buffered_reader_vfs_capabilities(buffered_reader_t *reader)
{
	return reader != 0 ? ppa_vfs_capabilities(reader->handle) : 0U;
}

int32_t buffered_reader_length(buffered_reader_t *reader)
{
	int64_t result = buffered_reader_length64(reader);
	return result > INT_MAX ? INT_MAX : (int32_t)result;
}

int32_t buffered_reader_seek(buffered_reader_t *reader, int32_t position)
{
	int64_t result;
	if (position < 0)
		position = 0;
	result = buffered_reader_seek64(reader, (uint64_t)(uint32_t)position);
	if (result < 0)
		return -1;
	return result > INT_MAX ? INT_MAX : (int32_t)result;
}

int32_t buffered_reader_position(buffered_reader_t *reader)
{
	int64_t result = buffered_reader_position64(reader);
	return result > INT_MAX ? INT_MAX : (int32_t)result;
}
