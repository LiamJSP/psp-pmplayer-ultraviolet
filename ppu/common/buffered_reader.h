#ifndef __COOLEYES_BUFFERED_READER_H__
#define __COOLEYES_BUFFERED_READER_H__

#include <psptypes.h>
#include <stdint.h>

#include "ppa_io_pump.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct buffered_reader_struct buffered_reader_t;

enum PpaReadWindowState {
	PPA_WINDOW_EMPTY = 0,
	PPA_WINDOW_FILLING,
	PPA_WINDOW_READY,
	PPA_WINDOW_IN_USE,
	PPA_WINDOW_ERROR,
	PPA_WINDOW_CANCELLED
};

struct PpaReadWindow {
	uint8_t *data;
	uint64_t file_offset;
	uint32_t valid_bytes;
	uint32_t capacity;
	uint32_t generation;
	uint32_t references;
	enum PpaReadWindowState state;
};

/*
 * A span is a synchronous, read-only view of bytes already published by one
 * reader window. It never crosses a window and must be released before seek,
 * reset, or close. Borrow advances the logical reader position exactly like a
 * successful read; release does not change the position.
 */
struct PpaBufferedReaderSpan {
	buffered_reader_t *reader;
	const uint8_t *data;
	uint32_t size;
	uint32_t generation;
	int32_t window_index;
	uint32_t active;
};

buffered_reader_t *buffered_reader_open(const char *path,
                                        int32_t buffer_size,
                                        int32_t seek_mode,
                                        uint32_t priority);

/* Session integration point. Multiple logical readers may share one pump;
 * each reader still owns a separate VFS cursor/descriptor and three windows. */
buffered_reader_t *buffered_reader_open_with_pump_at(
                                        const char *path,
                                        int32_t buffer_size,
                                        int32_t seek_mode,
                                        uint32_t priority,
                                        struct PpaIoPump *pump,
                                        uint32_t stream_id,
                                        uint64_t initial_position);

int64_t buffered_reader_length64(buffered_reader_t *reader);
int64_t buffered_reader_seek64(buffered_reader_t *reader, uint64_t position);
int64_t buffered_reader_position64(buffered_reader_t *reader);

/* Legacy signed-32 wrappers retained for old parser helpers. */
int32_t buffered_reader_length(buffered_reader_t *reader);
int32_t buffered_reader_seek(buffered_reader_t *reader, int32_t position);
int32_t buffered_reader_position(buffered_reader_t *reader);

uint32_t buffered_reader_read(buffered_reader_t *reader,
                              void *buffer,
                              uint32_t size);
int buffered_reader_read_exact(buffered_reader_t *reader,
                               void *buffer,
                               uint32_t size);

/*
 * Borrow the next size bytes only when they are contiguous in one published
 * window and meet the requested power-of-two alignment. readable_tail bytes
 * must also remain in that window, but their contents are not guaranteed to be
 * zero. This is therefore suitable for immediate demux parsing, not as a
 * decoder-padding or asynchronous packet-lifetime guarantee.
 */
int buffered_reader_borrow_current(buffered_reader_t *reader,
                                   uint32_t size,
                                   uint32_t alignment,
                                   uint32_t readable_tail,
                                   struct PpaBufferedReaderSpan *span);
void buffered_reader_release_span(struct PpaBufferedReaderSpan *span);

/* Update queued background windows without waking or blocking the parser.
 * not_before_us controls refill clustering; media_deadline_us controls ordering
 * against other logical streams on the shared pump. */
void buffered_reader_set_media_schedule(buffered_reader_t *reader,
                                        uint64_t media_deadline_us,
                                        uint64_t not_before_us);

/* Convenience policy for time/byte queue watermarks. Urgent queues refill now;
 * healthy queues defer the next burst until their low watermark approaches. */
void buffered_reader_set_queue_watermarks(buffered_reader_t *reader,
                                          uint32_t queued_duration_ms,
                                          uint32_t low_ms,
                                          uint32_t target_ms,
                                          uint32_t queued_bytes,
                                          uint32_t byte_ceiling);

uint32_t buffered_reader_generation(buffered_reader_t *reader);
uint32_t buffered_reader_vfs_capabilities(buffered_reader_t *reader);

void buffered_reader_close(buffered_reader_t *reader);

#ifdef __cplusplus
}
#endif

#endif
