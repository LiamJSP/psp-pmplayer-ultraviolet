#include "ppa_io.h"

#include <string.h>
#include <pspthreadman.h>

#define PPA_IO_CANCEL_SLOT_COUNT 8
#define PPA_IO_GATE_WAIT_US       5000U

typedef struct ppa_io_cancel_slot {
	volatile SceUID thread_id;
	ppa_io_cancel_check_fn check;
	void *user;
} ppa_io_cancel_slot;

static ppa_io_cancel_slot g_ppa_io_cancel_slots[PPA_IO_CANCEL_SLOT_COUNT];
static SceUID g_ppa_io_cancel_lock = -1;
static SceUID g_ppa_io_background_gate = -1;

static int ppa_io_cancel_lock(void)
{
	return g_ppa_io_cancel_lock >= 0 &&
	       sceKernelWaitSema(g_ppa_io_cancel_lock, 1, 0) >= 0;
}

static void ppa_io_cancel_unlock(void)
{
	if (g_ppa_io_cancel_lock >= 0)
		sceKernelSignalSema(g_ppa_io_cancel_lock, 1);
}

int ppa_io_background_init(void)
{
	if (g_ppa_io_cancel_lock < 0) {
		g_ppa_io_cancel_lock = sceKernelCreateSema(
			"ppa_io_cancel_lock", 0, 1, 1, 0);
		if (g_ppa_io_cancel_lock < 0)
			return 0;
	}
	if (g_ppa_io_background_gate < 0) {
		g_ppa_io_background_gate = sceKernelCreateSema(
			"ppa_io_background", 0, 1, 1, 0);
		if (g_ppa_io_background_gate < 0) {
			sceKernelDeleteSema(g_ppa_io_cancel_lock);
			g_ppa_io_cancel_lock = -1;
			return 0;
		}
	}
	return 1;
}

void ppa_io_background_shutdown(void)
{
	if (g_ppa_io_background_gate >= 0) {
		sceKernelDeleteSema(g_ppa_io_background_gate);
		g_ppa_io_background_gate = -1;
	}
	if (g_ppa_io_cancel_lock >= 0) {
		sceKernelDeleteSema(g_ppa_io_cancel_lock);
		g_ppa_io_cancel_lock = -1;
	}
	memset(g_ppa_io_cancel_slots, 0, sizeof(g_ppa_io_cancel_slots));
}

void ppa_io_set_cancel_check(ppa_io_cancel_check_fn check, void *user)
{
	SceUID thread_id = sceKernelGetThreadId();
	int free_slot = -1;
	int i;

	if (g_ppa_io_cancel_lock < 0 && !ppa_io_background_init())
		return;
	if (!ppa_io_cancel_lock())
		return;
	for (i = 0; i < PPA_IO_CANCEL_SLOT_COUNT; ++i) {
		if (g_ppa_io_cancel_slots[i].thread_id == thread_id) {
			if (check == 0) {
				/* Clear the callable pointer first; readers can scan lock-free. */
				g_ppa_io_cancel_slots[i].check = 0;
				g_ppa_io_cancel_slots[i].user = 0;
				g_ppa_io_cancel_slots[i].thread_id = 0;
			}
			else {
				g_ppa_io_cancel_slots[i].user = user;
				g_ppa_io_cancel_slots[i].check = check;
			}
			ppa_io_cancel_unlock();
			return;
		}
		if (free_slot < 0 && g_ppa_io_cancel_slots[i].thread_id == 0)
			free_slot = i;
	}
	if (check != 0 && free_slot >= 0) {
		g_ppa_io_cancel_slots[free_slot].user = user;
		g_ppa_io_cancel_slots[free_slot].thread_id = thread_id;
		/* Publish the function pointer last. */
		g_ppa_io_cancel_slots[free_slot].check = check;
	}
	ppa_io_cancel_unlock();
}

int ppa_io_cancel_requested(void)
{
	SceUID thread_id = sceKernelGetThreadId();
	int i;
	for (i = 0; i < PPA_IO_CANCEL_SLOT_COUNT; ++i) {
		ppa_io_cancel_check_fn check;
		if (g_ppa_io_cancel_slots[i].thread_id != thread_id)
			continue;
		check = g_ppa_io_cancel_slots[i].check;
		return check != 0 && check(g_ppa_io_cancel_slots[i].user);
	}
	return 0;
}

int ppa_io_background_acquire(ppa_io_cancel_check_fn check, void *user)
{
	if (g_ppa_io_background_gate < 0 && !ppa_io_background_init())
		return 0;
	for (;;) {
		SceUInt timeout = PPA_IO_GATE_WAIT_US;
		if (check != 0 && check(user))
			return 0;
		if (sceKernelWaitSema(g_ppa_io_background_gate, 1, &timeout) >= 0)
			return 1;
	}
}

void ppa_io_background_release(void)
{
	if (g_ppa_io_background_gate >= 0)
		sceKernelSignalSema(g_ppa_io_background_gate, 1);
}

static void ppa_io_delay_for_retry(unsigned int attempt)
{
	static const unsigned int delays_us[] = {
		1000,   /* 1 ms */
		3000,   /* 3 ms */
		8000,   /* 8 ms */
		16000   /* 16 ms */
	};

	if (attempt >= sizeof(delays_us) / sizeof(delays_us[0]))
		attempt = (sizeof(delays_us) / sizeof(delays_us[0])) - 1;

	if (!ppa_io_cancel_requested())
		sceKernelDelayThread(delays_us[attempt]);
}

static unsigned int ppa_io_read_chunk_size(unsigned int remaining)
{
	return remaining < PPA_IO_DEFAULT_CHUNK_SIZE ?
	       remaining : PPA_IO_DEFAULT_CHUNK_SIZE;
}

SceUID ppa_io_open_read_retry(const char *path, int flags, int mode)
{
	unsigned int attempt;
	SceUID fd = -1;

	if (path == 0)
		return -1;

	for (attempt = 0; attempt < PPA_IO_DEFAULT_RETRIES; attempt++) {
		if (ppa_io_cancel_requested())
			return -1;
		fd = sceIoOpen(path, flags, mode);
		if (fd >= 0)
			return fd;

		ppa_io_delay_for_retry(attempt);
	}

	return fd;
}

int ppa_io_seek32_retry(SceUID fd, int offset, int whence)
{
	unsigned int attempt;
	int result = -1;

	for (attempt = 0; attempt < PPA_IO_DEFAULT_RETRIES; attempt++) {
		if (ppa_io_cancel_requested())
			return -1;
		result = sceIoLseek32(fd, offset, whence);
		if (result == offset || whence != PSP_SEEK_SET)
			return result;

		ppa_io_delay_for_retry(attempt);
	}

	return result;
}

SceOff ppa_io_seek_retry(SceUID fd, SceOff offset, int whence)
{
	unsigned int attempt;
	SceOff result = -1;

	for (attempt = 0; attempt < PPA_IO_DEFAULT_RETRIES; attempt++) {
		if (ppa_io_cancel_requested())
			return -1;
		result = sceIoLseek(fd, offset, whence);
		if (result == offset || whence != PSP_SEEK_SET)
			return result;

		ppa_io_delay_for_retry(attempt);
	}

	return result;
}

int ppa_io_read_retry(SceUID fd, void *buffer, unsigned int size)
{
	unsigned char *out = (unsigned char *)buffer;
	unsigned int total = 0;
	unsigned int zero_reads = 0;
	unsigned int error_retries = 0;

	if (buffer == 0)
		return -1;

	while (total < size) {
		unsigned int chunk;
		int result;
		if (ppa_io_cancel_requested())
			return total != 0 ? (int)total : -1;
		chunk = ppa_io_read_chunk_size(size - total);
		result = sceIoRead(fd, out + total, chunk);

		if (result > 0) {
			error_retries = 0;
			total += (unsigned int)result;
			zero_reads = 0;

			continue;
		}

		if (result == 0) {
			zero_reads++;

			if (zero_reads >= PPA_IO_DEFAULT_RETRIES)
				break;

			ppa_io_delay_for_retry(zero_reads - 1);
			continue;
		}

		error_retries++;

		if (error_retries < PPA_IO_DEFAULT_RETRIES) {
			ppa_io_delay_for_retry(error_retries - 1);
			continue;
		}

		if (total == 0) {
			return result;
		}

		break;
	}

	return (int)total;
}

int ppa_io_read_exact_retry(SceUID fd, void *buffer, unsigned int size)
{
	int result = ppa_io_read_retry(fd, buffer, size);

	if (result == (int)size)
		return result;
	if (ppa_io_cancel_requested())
		return result;

	return result;
}

int ppa_io_read_exact_at32_retry(SceUID fd,
                                 int offset,
                                 void *buffer,
                                 unsigned int size)
{
	unsigned int attempt;

	for (attempt = 0; attempt < PPA_IO_DEFAULT_RETRIES; attempt++) {
		int result;

		if (ppa_io_cancel_requested())
			return -1;
		if (ppa_io_seek32_retry(fd, offset, PSP_SEEK_SET) != offset) {
			ppa_io_delay_for_retry(attempt);
			continue;
		}

		result = ppa_io_read_exact_retry(fd, buffer, size);
		if (result == (int)size)
			return result;

		ppa_io_delay_for_retry(attempt);
	}

	return -1;
}

int ppa_io_read_exact_at_retry(SceUID fd,
                               SceOff offset,
                               void *buffer,
                               unsigned int size)
{
	unsigned int attempt;

	for (attempt = 0; attempt < PPA_IO_DEFAULT_RETRIES; attempt++) {
		int result;

		if (ppa_io_cancel_requested())
			return -1;
		if (ppa_io_seek_retry(fd, offset, PSP_SEEK_SET) != offset) {
			ppa_io_delay_for_retry(attempt);
			continue;
		}

		result = ppa_io_read_exact_retry(fd, buffer, size);
		if (result == (int)size)
			return result;

		ppa_io_delay_for_retry(attempt);
	}

	return -1;
}

int ppa_io_async_read_start_at32_retry(SceUID fd,
                                       int offset,
                                       void *buffer,
                                       unsigned int size)
{
	unsigned int attempt;

	if (buffer == 0)
		return -1;

	for (attempt = 0; attempt < PPA_IO_DEFAULT_RETRIES; attempt++) {
		if (ppa_io_cancel_requested())
			return -1;
		if (ppa_io_seek32_retry(fd, offset, PSP_SEEK_SET) != offset) {
			ppa_io_delay_for_retry(attempt);
			continue;
		}

		if (sceIoReadAsync(fd, buffer, size) >= 0)
			return 0;

		ppa_io_delay_for_retry(attempt);
	}

	return -1;
}

int ppa_io_async_read_wait_exact_or_sync_at32(SceUID fd,
                                              int offset,
                                              void *buffer,
                                              unsigned int size)
{
	long long async_result = 0;
	int wait_result;

	if (buffer == 0 || ppa_io_cancel_requested())
		return -1;

	wait_result = sceIoWaitAsync(fd, &async_result);

	if (wait_result >= 0 && async_result == (long long)size)
		return (int)size;

	/*
	 * Some Memory Stick adapters fail async reads while sync reads still
	 * succeed. Re-read the entire requested region synchronously so parsers
	 * never consume a partly valid packet/block.
	 */

	return ppa_io_read_exact_at32_retry(fd, offset, buffer, size);
}

int ppa_io_async_read_exact_at_retry(SceUID fd,
                                     SceOff offset,
                                     void *buffer,
                                     unsigned int size,
                                     int *out_sync_fallback)
{
	unsigned int attempt;
	int started = 0;
	long long async_result = 0;
	int wait_result;

	if (out_sync_fallback != 0)
		*out_sync_fallback = 0;

	if (buffer == 0)
		return -1;
	if (size == 0U)
		return 0;

	for (attempt = 0; attempt < PPA_IO_DEFAULT_RETRIES; ++attempt) {
		if (ppa_io_cancel_requested())
			return -1;

		if (ppa_io_seek_retry(fd, offset, PSP_SEEK_SET) != offset) {
			ppa_io_delay_for_retry(attempt);
			continue;
		}

		if (sceIoReadAsync(fd, buffer, size) >= 0) {
			started = 1;
			break;
		}

		ppa_io_delay_for_retry(attempt);
	}

	if (started) {
		wait_result = sceIoWaitAsync(fd, &async_result);
		if (wait_result >= 0 && async_result == (long long)size)
			return (int)size;
	}

	if (ppa_io_cancel_requested())
		return -1;

	/* Async support varies by CFW and Memory Stick adapter. Re-read the entire
	 * range so a window is never published with a partly async-filled prefix. */
	if (out_sync_fallback != 0)
		*out_sync_fallback = 1;
	return ppa_io_read_exact_at_retry(fd, offset, buffer, size);
}
