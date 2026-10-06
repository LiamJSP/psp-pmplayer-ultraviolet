#ifndef PPA_IO_PUMP_H
#define PPA_IO_PUMP_H

#include <psptypes.h>
#include <pspkerneltypes.h>
#include <stdint.h>

#include "ppa_vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PPA_IO_PUMP_QUEUE_CAPACITY 16U

struct PpaIoPump;

enum PpaIoRequestState {
	PPA_IO_REQUEST_IDLE = 0,
	PPA_IO_REQUEST_QUEUED,
	PPA_IO_REQUEST_ACTIVE,
	PPA_IO_REQUEST_COMPLETE,
	PPA_IO_REQUEST_ERROR,
	PPA_IO_REQUEST_CANCELLED
};

enum PpaIoRequestFlags {
	PPA_IO_REQUEST_REQUIRED            = 1u << 0,
	PPA_IO_REQUEST_PREFER_NATIVE_ASYNC = 1u << 1
};

struct PpaIoRequest {
	ppa_vfs_file *file;
	uint64_t offset;
	void *destination;
	uint32_t bytes;
	uint64_t media_deadline_us;
	uint64_t not_before_us;
	uint32_t stream_id;
	uint32_t generation;
	uint32_t flags;
	void *owner;

	volatile uint32_t completed_bytes;
	volatile int result;
	volatile int state;
	volatile int cancel_requested;
	SceUID completion_sema;
};

struct PpaIoPump *ppa_io_pump_create(void);
void ppa_io_pump_destroy(struct PpaIoPump *pump);

int ppa_io_request_init(struct PpaIoRequest *request, const char *name);
void ppa_io_request_destroy(struct PpaIoRequest *request);

int ppa_io_pump_submit(struct PpaIoPump *pump,
                       struct PpaIoRequest *request,
                       ppa_vfs_file *file,
                       uint64_t offset,
                       void *destination,
                       uint32_t bytes,
                       uint64_t media_deadline_us,
                       uint64_t not_before_us,
                       uint32_t stream_id,
                       uint32_t generation,
                       void *owner,
                       uint32_t flags);

void ppa_io_pump_update_schedule(struct PpaIoPump *pump,
                                 struct PpaIoRequest *request,
                                 uint64_t media_deadline_us,
                                 uint64_t not_before_us);

/* Returns the request byte result, or a negative value for error/cancel. */
int ppa_io_request_wait(struct PpaIoRequest *request);

/* Cancelling with wait_for_worker=1 establishes that the destination is no
 * longer being written before the caller reuses or frees it. */
void ppa_io_pump_cancel(struct PpaIoPump *pump,
                        struct PpaIoRequest *request,
                        int wait_for_worker);

#ifdef __cplusplus
}
#endif

#endif
