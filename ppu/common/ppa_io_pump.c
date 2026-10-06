#include "ppa_io_pump.h"

#include "ppa_io.h"
#include "ppa_thread_policy.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#define PPA_IO_PUMP_STACK_BYTES       0x10000
#define PPA_IO_PUMP_DEADLINE_TIE_US   20000ULL

struct PpaIoPump {
	SceUID thread;
	SceUID lock;
	SceUID wake;
	volatile int stop;
	struct PpaIoRequest *queue[PPA_IO_PUMP_QUEUE_CAPACITY];
	uint32_t queue_count;
	struct PpaIoRequest *active;
	ppa_vfs_file *last_file;
	uint64_t last_end;
};

static uint64_t ppa_io_pump_now(void)
{
	return (uint64_t)sceKernelGetSystemTimeWide();
}

static int ppa_io_pump_lock(struct PpaIoPump *pump)
{
	return pump != 0 && pump->lock >= 0 &&
	       sceKernelWaitSema(pump->lock, 1, 0) >= 0;
}

static void ppa_io_pump_unlock(struct PpaIoPump *pump)
{
	if (pump != 0 && pump->lock >= 0)
		sceKernelSignalSema(pump->lock, 1);
}

static void ppa_io_pump_wake(struct PpaIoPump *pump)
{
	if (pump != 0 && pump->wake >= 0)
		(void)sceKernelSignalSema(pump->wake, 1);
}

static void ppa_io_request_drain_completion(struct PpaIoRequest *request)
{
	if (request == 0 || request->completion_sema < 0)
		return;

	while (sceKernelPollSema(request->completion_sema, 1) >= 0) {
		/* A request carries one completion token; loop defensively. */
	}
}

static void ppa_io_request_publish_completion(struct PpaIoRequest *request,
                                              int state,
                                              int result)
{
	if (request == 0)
		return;

	request->result = result;
	request->state = state;
	if (request->completion_sema >= 0)
		(void)sceKernelSignalSema(request->completion_sema, 1);
}

static int ppa_io_pump_cancel_check(void *user)
{
	struct PpaIoPump *pump = (struct PpaIoPump *)user;
	struct PpaIoRequest *request;

	if (pump == 0 || pump->stop)
		return 1;
	request = pump->active;
	return request != 0 && request->cancel_requested;
}

static void ppa_io_pump_remove_at(struct PpaIoPump *pump, uint32_t index)
{
	uint32_t i;

	if (pump == 0 || index >= pump->queue_count)
		return;
	for (i = index + 1U; i < pump->queue_count; ++i)
		pump->queue[i - 1U] = pump->queue[i];
	pump->queue_count--;
	pump->queue[pump->queue_count] = 0;
}

static uint64_t ppa_io_request_next_offset(const struct PpaIoRequest *request)
{
	return request->offset + (uint64_t)request->completed_bytes;
}

static int ppa_io_deadline_earlier(uint64_t a, uint64_t b)
{
	if (a == 0)
		return 0;
	if (b == 0)
		return 1;
	return a < b;
}

/* Select an eligible request. When deadlines are effectively equal, retain a
 * nearby contiguous range on the same descriptor to avoid needless seeks. */
static int ppa_io_pump_select(struct PpaIoPump *pump,
                              uint64_t now,
                              uint64_t *out_next_release)
{
	int best = -1;
	uint32_t i;
	uint64_t best_deadline = 0;
	int best_contiguous = 0;
	uint64_t next_release = 0;

	for (i = 0; i < pump->queue_count; ++i) {
		struct PpaIoRequest *request = pump->queue[i];
		int contiguous;
		uint64_t deadline;

		if (request == 0 || request->state != PPA_IO_REQUEST_QUEUED)
			continue;
		if (request->cancel_requested)
			continue;
		if (request->not_before_us > now) {
			if (next_release == 0 || request->not_before_us < next_release)
				next_release = request->not_before_us;
			continue;
		}

		deadline = request->media_deadline_us;
		contiguous = pump->last_file == request->file &&
		             pump->last_end == ppa_io_request_next_offset(request);

		if (best < 0 || ppa_io_deadline_earlier(deadline, best_deadline)) {
			best = (int)i;
			best_deadline = deadline;
			best_contiguous = contiguous;
			continue;
		}

		if (deadline != 0 && best_deadline != 0) {
			uint64_t delta = deadline > best_deadline ?
			                 deadline - best_deadline : best_deadline - deadline;
			if (delta <= PPA_IO_PUMP_DEADLINE_TIE_US &&
			    contiguous && !best_contiguous) {
				best = (int)i;
				best_deadline = deadline;
				best_contiguous = 1;
			}
		}
		else if (deadline == best_deadline && contiguous && !best_contiguous) {
			best = (int)i;
			best_contiguous = 1;
		}
	}

	if (out_next_release != 0)
		*out_next_release = next_release;
	return best;
}

static void ppa_io_pump_complete_active(struct PpaIoPump *pump,
                                        struct PpaIoRequest *request,
                                        int state,
                                        int result)
{
	if (!ppa_io_pump_lock(pump)) {
		ppa_io_request_publish_completion(request, state, result);
		return;
	}

	if (pump->active == request)
		pump->active = 0;
	ppa_io_pump_unlock(pump);

	ppa_io_request_publish_completion(request, state, result);
}

static void ppa_io_pump_process_burst(struct PpaIoPump *pump,
                                      struct PpaIoRequest *request)
{
	uint32_t remaining;
	uint32_t burst;
	int result;

	if (request->cancel_requested || pump->stop) {
		ppa_io_pump_complete_active(pump, request,
		                            PPA_IO_REQUEST_CANCELLED, -1);
		return;
	}

	remaining = request->bytes - request->completed_bytes;
	burst = ppa_vfs_recommended_burst(request->file);
	if (burst == 0U || burst > remaining)
		burst = remaining;

	result = ppa_vfs_worker_read_at64(
	             request->file,
	             request->offset + (uint64_t)request->completed_bytes,
	             (uint8_t *)request->destination + request->completed_bytes,
	             burst,
	             (request->flags & PPA_IO_REQUEST_PREFER_NATIVE_ASYNC) != 0U,
	             0,
	             0);

	if (request->cancel_requested || pump->stop) {
		ppa_io_pump_complete_active(pump, request,
		                            PPA_IO_REQUEST_CANCELLED, -1);
		return;
	}

	if (result != (int)burst) {
		ppa_io_pump_complete_active(pump, request,
		                            PPA_IO_REQUEST_ERROR,
		                            result < 0 ? result : -1);
		return;
	}

	request->completed_bytes += burst;

	if (request->completed_bytes == request->bytes) {
		pump->last_file = request->file;
		pump->last_end = request->offset + request->bytes;
		ppa_io_pump_complete_active(pump, request,
		                            PPA_IO_REQUEST_COMPLETE,
		                            (int)request->bytes);
		return;
	}

	/* Yield request ordering only at an efficient backend burst boundary. */
	if (!ppa_io_pump_lock(pump)) {
		ppa_io_pump_complete_active(pump, request,
		                            PPA_IO_REQUEST_ERROR, -1);
		return;
	}
	pump->last_file = request->file;
	pump->last_end = request->offset + request->completed_bytes;
	pump->active = 0;
	request->state = PPA_IO_REQUEST_QUEUED;
	if (pump->queue_count < PPA_IO_PUMP_QUEUE_CAPACITY) {
		pump->queue[pump->queue_count++] = request;
		ppa_io_pump_unlock(pump);
		ppa_io_pump_wake(pump);
		return;
	}
	ppa_io_pump_unlock(pump);
	ppa_io_request_publish_completion(request, PPA_IO_REQUEST_ERROR, -1);
}

static int ppa_io_pump_thread(SceSize args, void *argp)
{
	struct PpaIoPump *pump = 0;

	(void)args;
	if (argp != 0)
		pump = *(struct PpaIoPump **)argp;
	if (pump == 0)
		return -1;

	ppa_io_set_cancel_check(ppa_io_pump_cancel_check, pump);

	while (!pump->stop) {
		struct PpaIoRequest *request = 0;
		uint64_t now;
		uint64_t next_release = 0;
		int selected;

		now = ppa_io_pump_now();
		if (ppa_io_pump_lock(pump)) {
			selected = ppa_io_pump_select(pump, now, &next_release);
			if (selected >= 0) {
				request = pump->queue[selected];
				ppa_io_pump_remove_at(pump, (uint32_t)selected);
				pump->active = request;
				request->state = PPA_IO_REQUEST_ACTIVE;
			}
			ppa_io_pump_unlock(pump);
		}

		if (request != 0) {
			ppa_io_pump_process_burst(pump, request);
			continue;
		}

		if (pump->stop)
			break;

		if (next_release != 0 && next_release > now) {
			uint64_t delta = next_release - now;
			SceUInt timeout = delta > (uint64_t)UINT_MAX ?
			                  UINT_MAX : (SceUInt)delta;
			(void)sceKernelWaitSema(pump->wake, 1, &timeout);
		}
		else {
			(void)sceKernelWaitSema(pump->wake, 1, 0);
		}
	}

	ppa_io_set_cancel_check(0, 0);
	return 0;
}

struct PpaIoPump *ppa_io_pump_create(void)
{
	struct PpaIoPump *pump;
	struct PpaIoPump *thread_arg;

	pump = (struct PpaIoPump *)malloc(sizeof(*pump));
	if (pump == 0)
		return 0;
	memset(pump, 0, sizeof(*pump));
	pump->thread = -1;
	pump->lock = -1;
	pump->wake = -1;

	pump->lock = sceKernelCreateSema("ppa_io_pump_lock", 0, 1, 1, 0);
	if (pump->lock < 0)
		goto fail;
	pump->wake = sceKernelCreateSema("ppa_io_pump_wake", 0, 0, 1, 0);
	if (pump->wake < 0)
		goto fail;

	pump->thread = ppa_thread_create(PPA_THREAD_PLAYBACK_IO,
	                                 "ppa_playback_io",
	                                 ppa_io_pump_thread,
	                                 PPA_IO_PUMP_STACK_BYTES, 0);
	if (pump->thread < 0)
		goto fail;

	thread_arg = pump;
	if (sceKernelStartThread(pump->thread,
	                         sizeof(thread_arg), &thread_arg) < 0)
		goto fail;

	return pump;

fail:
	if (pump->thread >= 0) {
		sceKernelDeleteThread(pump->thread);
		pump->thread = -1;
	}
	if (pump->wake >= 0)
		sceKernelDeleteSema(pump->wake);
	if (pump->lock >= 0)
		sceKernelDeleteSema(pump->lock);
	free(pump);
	return 0;
}

void ppa_io_pump_destroy(struct PpaIoPump *pump)
{
	uint32_t i;

	if (pump == 0)
		return;

	if (ppa_io_pump_lock(pump)) {
		pump->stop = 1;
		for (i = 0; i < pump->queue_count; ++i) {
			struct PpaIoRequest *request = pump->queue[i];
			if (request != 0) {
				request->cancel_requested = 1;
				ppa_io_request_publish_completion(
				    request, PPA_IO_REQUEST_CANCELLED, -1);
			}
		}
		pump->queue_count = 0;
		if (pump->active != 0)
			pump->active->cancel_requested = 1;
		ppa_io_pump_unlock(pump);
	}
	else {
		pump->stop = 1;
	}

	ppa_io_pump_wake(pump);
	if (pump->thread >= 0) {
		(void)sceKernelWaitThreadEnd(pump->thread, 0);
		sceKernelDeleteThread(pump->thread);
	}
	if (pump->wake >= 0)
		sceKernelDeleteSema(pump->wake);
	if (pump->lock >= 0)
		sceKernelDeleteSema(pump->lock);

	free(pump);
}

int ppa_io_request_init(struct PpaIoRequest *request, const char *name)
{
	const char *safe_name = name != 0 ? name : "ppa_io_done";

	if (request == 0)
		return 0;
	memset(request, 0, sizeof(*request));
	request->completion_sema = sceKernelCreateSema(safe_name, 0, 0, 1, 0);
	if (request->completion_sema < 0) {
		request->completion_sema = -1;
		return 0;
	}
	request->state = PPA_IO_REQUEST_IDLE;
	return 1;
}

void ppa_io_request_destroy(struct PpaIoRequest *request)
{
	if (request == 0)
		return;
	if (request->completion_sema >= 0)
		sceKernelDeleteSema(request->completion_sema);
	memset(request, 0, sizeof(*request));
	request->completion_sema = -1;
}

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
                       uint32_t flags)
{
	if (pump == 0 || request == 0 || file == 0 || destination == 0 ||
	    bytes == 0U || request->completion_sema < 0)
		return 0;

	ppa_io_request_drain_completion(request);
	if (!ppa_io_pump_lock(pump))
		return 0;
	/* Keep one queue slot available for the active request to yield at a
	 * physical-burst boundary. This makes queue saturation lossless instead of
	 * turning a valid in-flight window into an artificial I/O failure. */
	if (pump->stop ||
	    pump->queue_count >= PPA_IO_PUMP_QUEUE_CAPACITY -
	                         (pump->active != 0 ? 1U : 0U) ||
	    request->state == PPA_IO_REQUEST_QUEUED ||
	    request->state == PPA_IO_REQUEST_ACTIVE) {
		ppa_io_pump_unlock(pump);
		return 0;
	}

	request->file = file;
	request->offset = offset;
	request->destination = destination;
	request->bytes = bytes;
	request->media_deadline_us = media_deadline_us;
	request->not_before_us = not_before_us;
	request->stream_id = stream_id;
	request->generation = generation;
	request->flags = flags;
	request->owner = owner;
	request->completed_bytes = 0;
	request->result = 0;
	request->cancel_requested = 0;
	request->state = PPA_IO_REQUEST_QUEUED;

	pump->queue[pump->queue_count++] = request;
	ppa_io_pump_unlock(pump);
	ppa_io_pump_wake(pump);
	return 1;
}

void ppa_io_pump_update_schedule(struct PpaIoPump *pump,
                                 struct PpaIoRequest *request,
                                 uint64_t media_deadline_us,
                                 uint64_t not_before_us)
{
	if (pump == 0 || request == 0)
		return;
	if (!ppa_io_pump_lock(pump))
		return;
	if (request->state == PPA_IO_REQUEST_QUEUED) {
		request->media_deadline_us = media_deadline_us;
		request->not_before_us = not_before_us;
	}
	ppa_io_pump_unlock(pump);
	ppa_io_pump_wake(pump);
}

static int ppa_io_request_terminal(int state)
{
	return state == PPA_IO_REQUEST_COMPLETE ||
	       state == PPA_IO_REQUEST_ERROR ||
	       state == PPA_IO_REQUEST_CANCELLED;
}

int ppa_io_request_wait(struct PpaIoRequest *request)
{
	int state;

	if (request == 0 || request->completion_sema < 0)
		return -1;
	state = request->state;
	if (state == PPA_IO_REQUEST_IDLE)
		return -1;
	/* Completion is published result-first/state-second. Returning directly for
	 * a terminal request makes cancellation and teardown waits idempotent; any
	 * unconsumed token is drained before the descriptor is submitted again. */
	if (ppa_io_request_terminal(state))
		return request->result;
	if (sceKernelWaitSema(request->completion_sema, 1, 0) < 0)
		return -1;
	return request->result;
}

void ppa_io_pump_cancel(struct PpaIoPump *pump,
                        struct PpaIoRequest *request,
                        int wait_for_worker)
{
	int publish_cancel = 0;
	int should_wait = 0;
	uint32_t i;

	if (pump == 0 || request == 0)
		return;

	if (ppa_io_pump_lock(pump)) {
		if (request->state == PPA_IO_REQUEST_QUEUED) {
			for (i = 0; i < pump->queue_count; ++i) {
				if (pump->queue[i] == request) {
					ppa_io_pump_remove_at(pump, i);
					break;
				}
			}
			request->cancel_requested = 1;
			publish_cancel = 1;
		}
		else if (request->state == PPA_IO_REQUEST_ACTIVE) {
			request->cancel_requested = 1;
			should_wait = wait_for_worker;
		}
		ppa_io_pump_unlock(pump);
	}

	if (publish_cancel)
		ppa_io_request_publish_completion(request,
		                               PPA_IO_REQUEST_CANCELLED, -1);
	ppa_io_pump_wake(pump);
	if (should_wait)
		(void)ppa_io_request_wait(request);
}
