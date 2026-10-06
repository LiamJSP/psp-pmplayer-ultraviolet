#include <new>
#include "mod/subtitle_font_config.h"
#include "common/ppa_process.h"
// player.cpp
#include <strings.h>
#ifndef stricmp
#define stricmp strcasecmp
#endif
#ifndef strnicmp
#define strnicmp strncasecmp
#endif
/*
 *	Copyright (C) 2006 cooleyes
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
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <psputils.h>
#include <psprtc.h>
#include <pspctrl.h>
#include <psppower.h>
#include <pspdebug.h>

#include "player.h"
#include <boost/smart_ptr/scoped_ptr.hpp>
#include "common/ppa_playback_session.h"
#include "media/PlaybackUi.h"
#include "config.h"
#include "skin.h"
#include "ftfont.h"
#include "ui_i18n.h"
#include "versiondialog.h"
#include "cfgdialog.h"
#include "msgdialog.h"
#include "common/ppu_controls.h"
#include "videomode.h"

#include "common/libminiconv.h"
#include "common/libi18n.h"
#include "common/fat.h"
#include "common/directory.h"
#include "common/ctrl.h"
#include "common/imagefile.h"
#include "common/base64.h"
#include "common/m33sdk.h"
#include "common/ppa_hardware_profile.h"
#include "common/ppa_vfs.h"
#include "common/ppa_ntfs.h"
#include "common/ppa_thread_policy.h"
#include "common/ppa_scratchpad.h"
#include "common/ppa_io.h"
#include "common/ppa_browser_fs_cache.h"
#include "common/ppa_playback_control.h"
#include "bufferedio.h"

#include "mod/subtitle_charset.h"
#include "mod/subtitle_preferences.h"
#include "mod/cpu_clock.h"
#include "mod/mp4avcdecoder.h"
#include "mod/psp1k_frame_buffer.h"
#include "mod/gu_draw.h"

#include "mod/codec_prx.h"
#include "mod/gu_font.h"
#include "mod/movie_file.h"
#include "common/ppa_utf8.h"
#include "mod/movie_stat.h"

#include "common/ppa_memory.h"
#include "common/ppa_privileged_bridge.h"
#include "common/ppa_cache.h"
#include "media/MediaBackend.hpp"
#include "media/VideoPipeline.h"
#include "browser_text_cache.h"

#define TEXT_ITEM_BORDER 1

/* Full-brightness hold excludes the two fades. Directory startup may extend
 * the hold by at most three seconds, never trap the responsive browser behind
 * a stalled firmware/host I/O call. No new worker or clock owner is created. */
#define PPA_STARTUP_CREDIT_FADE_US 400000ULL
#define PPA_STARTUP_CREDIT_HOLD_US 2000000ULL
#define PPA_STARTUP_CREDIT_READY_LIMIT_US 5000000ULL

#ifndef PPA_ENABLE_THUMBNAILS
#define PPA_ENABLE_THUMBNAILS 0
#endif

static void ppa_bootlog_module_result(const char *phase, const char *path, int result, int status)
{
	char line[1280];
	const char *safe_phase = phase ? phase : "unknown";
	const char *safe_path = path ? path : "(null)";

	snprintf(line, sizeof(line),
	         "PPA_BOOT:module_%s,path=%s,result=%ld,hex=0x%08lx,status=%ld",
	         safe_phase, safe_path, (long)result,
	         (unsigned long)(unsigned int)result, (long)status);
}

static int ppa_boot_required_module(const char *path, SceUID *module_id_out)
{
	SceUID module_id;
	int status = 0;
	int start_result;

	if (module_id_out != NULL)
		*module_id_out = -1;
	module_id = m33KernelLoadModule(path, 0, NULL);
	if (module_id < 0) {
		ppa_bootlog_module_result("load_failed", path, (int)module_id, 0);
		return 0;
	}
	ppa_bootlog_module_result("loaded", path, (int)module_id, 0);

	start_result = sceKernelStartModule(module_id, 0, 0, &status, NULL);
	if (start_result < 0) {
		ppa_bootlog_module_result("start_failed", path, start_result, status);
		return 0;
	}
	ppa_bootlog_module_result("started", path, start_result, status);
	if (module_id_out != NULL)
		*module_id_out = module_id;
	return 1;
}

static void ppa_boot_fail_screen(const char *msg)
{
	pspDebugScreenInit();
	pspDebugScreenSetXY(0, 0);
	pspDebugScreenPrintf("PMPlayer Ultraviolet boot failed\n\n");
	pspDebugScreenPrintf("%s\n\n", msg ? msg : "(no message)");
	pspDebugScreenPrintf("Press X to exit.\n");

	while (1) {
		SceCtrlData pad;
		sceCtrlReadBufferPositive(&pad, 1);

		if (pad.Buttons & PSP_CTRL_CROSS)
			break;

		sceKernelDelayThread(50000);
	}
}

static void ppa_battery_saver_rejection_screen(Image *surface)
{
	static const char message[] =
		"Turn off Battery Saver Mode in Config to Play This File";
	FtFont *font;
	uint64_t deadline;
	int font_size;
	int x;

	if (surface == 0)
		return;
	FtFontManager *manager = FtFontManager::getInstance();
	font = manager ? manager->getMainFont() : NULL;
	font_size = font ? font->getPixelSize() : 12;
	clearImage(surface, 0xff000000U);
	if (font != 0) {
		x = (PSP_SCREEN_WIDTH -
		     (int)strlen(message) * font_size / 2) / 2;
		if (x < 4) x = 4;
		font->printStringToImage(surface, x,
		                         PSP_SCREEN_HEIGHT / 2 + font_size / 2,
		                         PSP_SCREEN_WIDTH - x - 4, font_size + 4,
		                         0x00ffffffU, message);
	}
	guStart();
	clearScreen();
	blitImageToScreen(0, 0, surface->imageWidth, surface->imageHeight,
	                  surface, 0, 0);
	flipScreen();

	ctrl_flush();
	deadline = (uint64_t)sceKernelGetSystemTimeWide() + 10000000ULL;
	while ((uint64_t)sceKernelGetSystemTimeWide() < deadline) {
		if (ctrl_wait(20000) & PSP_CTRL_TRIANGLE)
			break;
	}
	ctrl_flush();
}

static int ppa_playback_result_is_manual_exit(const char *result)
{
	return result != 0 && strcmp(result, "exit: manual") == 0;
}

static void ppa_playback_error_screen(const char *result)
{
	char body[768];
	uint64_t deadline;
	int draw_result;

	if (result == 0 || ppa_playback_result_is_manual_exit(result))
		return;
	snprintf(body, sizeof(body),
	         "Playback stopped before returning to the browser.\n"
	         "Reason: %.620s", result);
	draw_result = ppa_gu_draw_recovery_text_panel_blocking(
		"Playback error - recovered", body,
		"Triangle or X: return to browser");
	
	if (draw_result < 0)
		return;

	ctrl_flush();
	deadline = (uint64_t)sceKernelGetSystemTimeWide() + 10000000ULL;
	while ((uint64_t)sceKernelGetSystemTimeWide() < deadline) {
		unsigned int buttons = ctrl_wait(20000);
		if ((buttons & (PSP_CTRL_TRIANGLE | PSP_CTRL_CROSS)) != 0U)
			break;
	}
	ctrl_flush();
}

static void ppa_handle_playback_result(Image *surface,
                                       const char *path,
                                       const char *result)
{
	if (result == 0)
		return;
	if (strcmp(result, PPA_BATTERY_SAVER_REJECTED) == 0)
		ppa_battery_saver_rejection_screen(surface);
	else
		ppa_playback_error_screen(result);
}

struct movie_file_struct currentMovie;

struct subtitle_ext_charset_struct subtitleExt[] = {
	{".srt", 4, "DEFAULT"},
	{".sub", 4, "DEFAULT"},
	{".ass", 4, "DEFAULT"},
	{".ssa", 4, "DEFAULT"},
	{".en.srt", 7, "UTF-8"},
	{".ch.srt", 7, "GBK"},
	{".eng.srt", 8, "UTF-8"},
	{".gb.srt", 7, "GBK"},
	{".chs.srt", 8, "GBK"},
	{".sc.srt", 7, "GBK"},
	{".big5.srt", 9, "BIG5"},
	{".cht.srt", 8, "BIG5"},
	{".tc.srt", 7, "BIG5"},
	{".en.ass", 7, "UTF-8"},
	{".ch.ass", 7, "GBK"},
	{".eng.ass", 8, "UTF-8"},
	{".gb.ass", 7, "GBK"},
	{".chs.ass", 8, "GBK"},
	{".sc.ass", 7, "GBK"},
	{".big5.ass", 9, "BIG5"},
	{".cht.ass", 8, "BIG5"},
	{".tc.ass", 7, "BIG5"},
	{".en.ssa", 7, "UTF-8"},
	{".ch.ssa", 7, "GBK"},
	{".eng.ssa", 8, "UTF-8"},
	{".gb.ssa", 7, "GBK"},
	{".chs.ssa", 8, "GBK"},
	{".sc.ssa", 7, "GBK"},
	{".big5.ssa", 9, "BIG5"},
	{".cht.ssa", 8, "BIG5"},
	{".tc.ssa", 7, "BIG5"},
	{NULL, 0, NULL}
};

file_type_ext_struct movieFileFilter[] = {
	{"mp4", FS_MP4_FILE},
	{"mkv", FS_MKV_FILE},
	{NULL, FS_UNKNOWN_FILE}
};

file_type_ext_struct movieSubtitleFilter[] = {
	{"sub", FS_SUB_FILE},
	{"srt", FS_SRT_FILE},
	{"ass", FS_ASS_FILE},
	{"ssa", FS_SSA_FILE},
	{NULL, FS_UNKNOWN_FILE}
};

file_type_ext_struct movieAttachmentFilter[] = {
	{"sub", FS_SUB_FILE},
	{"srt", FS_SRT_FILE},
	{"ass", FS_ASS_FILE},
	{"ssa", FS_SSA_FILE},
	{"png", FS_PNG_FILE},
	{NULL, FS_UNKNOWN_FILE}
};

#define PPA_BROWSER_IDLE_IO_LIMIT_US 10000000U
#define PPA_BROWSER_METADATA_DWELL_US 2000000ULL
/* UI-owned activity window, read atomically by the two browser workers. The
 * latched pause prevents the low microsecond clock wrapping from reopening
 * storage after a long idle. Rendering shares this window: FreeType stream
 * reads during an otherwise unchanged repaint are also card activity. */
static volatile unsigned int g_ppa_browser_last_input_us;
static volatile unsigned int g_ppa_browser_idle_io_paused;

static void ppa_browser_note_activity(void)
{
	__sync_lock_test_and_set(&g_ppa_browser_last_input_us,
	                         sceKernelGetSystemTimeLow());
	__sync_lock_test_and_set(&g_ppa_browser_idle_io_paused, 0U);
}

static int ppa_browser_idle_io_expired(void)
{
	unsigned int last = __sync_fetch_and_add(&g_ppa_browser_last_input_us, 0U);
	return __sync_fetch_and_add(&g_ppa_browser_idle_io_paused, 0U) != 0U ||
	       (unsigned int)(sceKernelGetSystemTimeLow() - last) >=
	           PPA_BROWSER_IDLE_IO_LIMIT_US;
}

static int ppa_browser_input_settled(void)
{
	unsigned int last = __sync_fetch_and_add(&g_ppa_browser_last_input_us, 0U);
	/* Include held state, not only the sampler's repeat events. Optional I/O
	 * may start only after two complete seconds without controller activity.
	 * The sampler clock covers releases and events not dispatched by the UI. */
	return !ctrl_pending() && ctrl_read_cont() == 0 &&
	       ctrl_idle_time_us() >= (unsigned int)PPA_BROWSER_METADATA_DWELL_US &&
	       (unsigned int)(sceKernelGetSystemTimeLow() - last) >=
	           (unsigned int)PPA_BROWSER_METADATA_DWELL_US;
}

static unsigned int ppa_browser_cancel_generation(volatile unsigned int *value)
{
	unsigned int next = __sync_add_and_fetch(value, 1U);
	if (next == 0U)
		next = __sync_add_and_fetch(value, 1U);
	return next;
}

/* Directory enumeration owns its result buffer and never touches the browser's
 * active list. The UI can therefore continue draining controller events while
 * FAT/NTFS I/O runs at background priority. A generation change is a
 * safe cancellation point observed by ppa_io and per-entry directory loops. */
typedef struct ppa_browser_directory_task_struct {
	unsigned int generation;
	int refresh;
	int show_hidden;
	int show_unknown;
	char path[512];
	char short_path[512];
	char restore_name[256];
} ppa_browser_directory_task;

typedef struct ppa_browser_directory_result_struct {
	unsigned int generation;
	int refresh;
	int item_count;
	directory_item_struct *items;
	char path[512];
	char short_path[512];
	char restore_name[256];
} ppa_browser_directory_result;

typedef struct ppa_browser_directory_worker_struct {
	SceUID lock;
	SceUID wake;
	SceUID thread;
	volatile int initialized;
	volatile int stop;
	volatile unsigned int generation;
	volatile unsigned int active_generation;
	int pending;
	int running;
	int result_ready;
	ppa_browser_directory_task task;
	ppa_browser_directory_result result;
} ppa_browser_directory_worker;

static ppa_browser_directory_worker g_ppa_browser_directory_worker;
/* UI-owned latest request: a busy worker bookkeeping lock never stalls input
 * or loses navigation. The UI retries transfer with a poll on each loop. */
static ppa_browser_directory_task g_ppa_browser_directory_submission;
static int g_ppa_browser_directory_submission_pending;

#if PPA_ENABLE_THUMBNAILS
/* Preview PNG decoding remains a main-thread operation because the legacy
 * image loader is not documented as re-entrant. Delay it until navigation has
 * settled so rapid D-pad input never pays storage/decode cost for files the
 * user only passed over. */
#define PPA_BROWSER_PREVIEW_DWELL_US PPA_BROWSER_METADATA_DWELL_US
static char g_ppa_browser_preview_key[640];
static u64 g_ppa_browser_preview_due_us;
#endif

static int ppa_browser_directory_worker_lock(void)
{
	return g_ppa_browser_directory_worker.initialized &&
	       sceKernelWaitSema(g_ppa_browser_directory_worker.lock, 1, 0) >= 0;
}

static int ppa_browser_directory_worker_try_lock(void)
{
	return g_ppa_browser_directory_worker.initialized &&
	       sceKernelPollSema(g_ppa_browser_directory_worker.lock, 1) >= 0;
}

static void ppa_browser_directory_worker_unlock(void)
{
	if (g_ppa_browser_directory_worker.initialized)
		sceKernelSignalSema(g_ppa_browser_directory_worker.lock, 1);
}

static int ppa_browser_directory_worker_idle(void)
{
	int idle = 1;
	if (g_ppa_browser_directory_submission_pending)
		return 0;
	if (g_ppa_browser_directory_worker.initialized) {
		if (!ppa_browser_directory_worker_try_lock())
			return 0;
		idle = !g_ppa_browser_directory_worker.pending &&
		       !g_ppa_browser_directory_worker.running &&
		       !g_ppa_browser_directory_worker.result_ready;
		ppa_browser_directory_worker_unlock();
	}
	return idle;
}

static int ppa_browser_directory_worker_cancel_check(void *user)
{
	ppa_browser_directory_worker *worker =
		(ppa_browser_directory_worker *)user;
	if (worker == 0)
		return 1;
	/* Every listing, including a cold folder, waits for two seconds of input
	 * idle. Park at I/O/entry boundaries and retain the scan cursor on activity;
	 * a different navigation generation still cancels immediately. A worker
	 * thread is not a second CPU and must not start card I/O between repeats. */
	for (;;) {
		if (worker->stop || __sync_fetch_and_add(&worker->generation, 0U) !=
		                    worker->active_generation)
			return 1;
		if (!ppa_browser_idle_io_expired() && ppa_browser_input_settled())
			return 0;
		sceKernelDelayThread(50000U);
	}
}

static int ppa_browser_directory_worker_thread(SceSize args, void *argp)
{
	ppa_browser_directory_worker *worker = &g_ppa_browser_directory_worker;
	(void)args;
	(void)argp;

	while (worker != 0) {
		ppa_browser_directory_task task;
		ppa_browser_directory_result result;
		char mutable_short_path[512];

		if (sceKernelWaitSema(worker->wake, 1, 0) < 0)
			break;
		if (!ppa_browser_directory_worker_lock())
			break;
		if (worker->stop) {
			ppa_browser_directory_worker_unlock();
			break;
		}
		if (!worker->pending) {
			ppa_browser_directory_worker_unlock();
			continue;
		}

		task = worker->task;
		worker->pending = 0;
		worker->running = 1;
		worker->active_generation = task.generation;
		ppa_browser_directory_worker_unlock();

		memset(&result, 0, sizeof(result));
		result.generation = task.generation;
		result.refresh = task.refresh;
		memcpy(result.path, task.path, sizeof(result.path));
		memcpy(result.restore_name, task.restore_name,
		       sizeof(result.restore_name));
		memcpy(mutable_short_path, task.short_path,
		       sizeof(mutable_short_path));
		mutable_short_path[sizeof(mutable_short_path) - 1] = 0;

		if (ppa_io_background_acquire(
		        ppa_browser_directory_worker_cancel_check, worker)) {
			ppa_io_set_cancel_check(
				ppa_browser_directory_worker_cancel_check, worker);
			result.item_count = open_directory_names_only(
				task.path, mutable_short_path,
				task.show_hidden, task.show_unknown,
				movieFileFilter, &result.items);
			ppa_io_set_cancel_check(0, 0);
			ppa_io_background_release();
		}
		memcpy(result.short_path, mutable_short_path, sizeof(result.short_path));
		result.short_path[sizeof(result.short_path) - 1] = 0;

		/* Encode/cache the listing on this low-priority worker, never while the
		 * UI is trying to consume controller events. The active browser list is
		 * still published only after the generation check below. */
		if (result.item_count > 0 && result.items != 0 &&
		    !ppa_browser_directory_worker_cancel_check(worker)) {
			ppa_io_set_cancel_check(
				ppa_browser_directory_worker_cancel_check, worker);
			(void)ppa_browser_fs_cache_put(result.path, result.short_path,
			                              result.items, result.item_count, 1);
			ppa_io_set_cancel_check(0, 0);
		}

		if (!ppa_browser_directory_worker_lock()) {
			if (result.items != 0) free(result.items);
			break;
		}
		worker->running = 0;
		if (!worker->stop && __sync_fetch_and_add(&worker->generation, 0U) ==
		                     task.generation) {
			if (worker->result_ready && worker->result.items != 0)
				free(worker->result.items);
			worker->result = result;
			worker->result_ready = 1;
			result.items = 0;
		}
		ppa_browser_directory_worker_unlock();
		if (result.items != 0)
			free(result.items);
		ppa_browser_fs_cache_finish_interactive_io(task.generation);
	}

	ppa_io_set_cancel_check(0, 0);
	return 0;
}

static int ppa_browser_directory_worker_init(void)
{
	ppa_browser_directory_worker *worker = &g_ppa_browser_directory_worker;
	if (worker->initialized)
		return 1;
	memset(worker, 0, sizeof(*worker));
	worker->lock = worker->wake = worker->thread = -1;
	worker->lock = sceKernelCreateSema("ppa_browser_dir_lock", 0, 1, 1, 0);
	worker->wake = sceKernelCreateSema("ppa_browser_dir_wake", 0, 0, 1, 0);
	if (worker->lock < 0 || worker->wake < 0)
		goto fail;
	worker->initialized = 1;
	worker->generation = 1;
	worker->thread = ppa_thread_create(PPA_THREAD_BROWSER_DIRECTORY,
	                                   "ppa_browser_directory",
	                                   ppa_browser_directory_worker_thread,
	                                   64 * 1024, 0);
	if (worker->thread < 0 ||
	    sceKernelStartThread(worker->thread, 0, 0) < 0)
		goto fail;
	return 1;

fail:
	if (worker->thread >= 0) sceKernelDeleteThread(worker->thread);
	if (worker->wake >= 0) sceKernelDeleteSema(worker->wake);
	if (worker->lock >= 0) sceKernelDeleteSema(worker->lock);
	memset(worker, 0, sizeof(*worker));
	worker->lock = worker->wake = worker->thread = -1;
	return 0;
}

static void ppa_browser_directory_worker_cancel(int wait_for_idle)
{
	ppa_browser_directory_worker *worker = &g_ppa_browser_directory_worker;
	int running;
	g_ppa_browser_directory_submission_pending = 0;
	if (!worker->initialized)
		return;
	/* Invalidate before taking the bookkeeping semaphore. Navigation must
	 * never wait for a worker that currently owns a parser/storage call. */
	(void)ppa_browser_cancel_generation(&worker->generation);
	if (wait_for_idle ? ppa_browser_directory_worker_lock() :
	                    ppa_browser_directory_worker_try_lock()) {
		worker->pending = 0;
		if (worker->result_ready && worker->result.items != 0)
			free(worker->result.items);
		memset(&worker->result, 0, sizeof(worker->result));
		worker->result_ready = 0;
		ppa_browser_directory_worker_unlock();
	}
	ppa_browser_fs_cache_clear_interactive_io();
	if (!wait_for_idle)
		return;
	do {
		running = 0;
		if (ppa_browser_directory_worker_lock()) {
			running = worker->running;
			ppa_browser_directory_worker_unlock();
		}
		if (running) sceKernelDelayThread(1000);
	} while (running);
}

static void ppa_browser_directory_worker_shutdown(void)
{
	ppa_browser_directory_worker *worker = &g_ppa_browser_directory_worker;
	if (!worker->initialized)
		return;
	ppa_browser_directory_worker_cancel(1);
	if (ppa_browser_directory_worker_lock()) {
		worker->stop = 1;
		ppa_browser_directory_worker_unlock();
	}
	sceKernelSignalSema(worker->wake, 1);
	if (worker->thread >= 0) {
		sceKernelWaitThreadEnd(worker->thread, 0);
		sceKernelDeleteThread(worker->thread);
	}
	if (worker->wake >= 0) sceKernelDeleteSema(worker->wake);
	if (worker->lock >= 0) sceKernelDeleteSema(worker->lock);
	memset(worker, 0, sizeof(*worker));
	worker->lock = worker->wake = worker->thread = -1;
}

static void ppa_browser_directory_worker_submit(void)
{
	ppa_browser_directory_worker *worker = &g_ppa_browser_directory_worker;
	directory_item_struct *discard = 0;
	if (!g_ppa_browser_directory_submission_pending ||
	    !ppa_browser_directory_worker_try_lock())
		return;
	worker->task = g_ppa_browser_directory_submission;
	worker->pending = 1;
	discard = worker->result.items;
	memset(&worker->result, 0, sizeof(worker->result));
	worker->result_ready = 0;
	g_ppa_browser_directory_submission_pending = 0;
	ppa_browser_directory_worker_unlock();
	if (discard != 0) free(discard);
	sceKernelSignalSema(worker->wake, 1);
}

static int ppa_browser_directory_worker_schedule(const char *path,
                                                 const char *short_path,
                                                 int show_hidden,
                                                 int show_unknown,
                                                 const char *restore_name,
                                                 int refresh)
{
	ppa_browser_directory_worker *worker = &g_ppa_browser_directory_worker;
	ppa_browser_directory_task *task = &g_ppa_browser_directory_submission;
	if (!ppa_browser_directory_worker_init())
		return 0;
	if (path == 0) path = "";
	if (short_path == 0) short_path = path;
	if (restore_name == 0) restore_name = "";
	if (strlen(path) >= sizeof(worker->task.path) ||
	    strlen(short_path) >= sizeof(worker->task.short_path) ||
	    strlen(restore_name) >= sizeof(worker->task.restore_name))
		return 0;
	memset(task, 0, sizeof(*task));
	task->generation = ppa_browser_cancel_generation(&worker->generation);
	task->refresh = refresh;
	task->show_hidden = show_hidden;
	task->show_unknown = show_unknown;
	strcpy(task->path, path);
	strcpy(task->short_path, short_path);
	strcpy(task->restore_name, restore_name);
	g_ppa_browser_directory_submission_pending = 1;
	ppa_browser_fs_cache_request_interactive_io(task->generation);
	ppa_browser_directory_worker_submit();
	return 1;
}

static int ppa_browser_directory_worker_take_result(
	ppa_browser_directory_result *out)
{
	int available = 0;
	directory_item_struct *discard = 0;
	if (out == 0) return 0;
	memset(out, 0, sizeof(*out));
	if (ppa_browser_directory_worker_try_lock()) {
		if (g_ppa_browser_directory_worker.result_ready &&
		    g_ppa_browser_directory_worker.result.generation ==
		        __sync_fetch_and_add(&g_ppa_browser_directory_worker.generation, 0U)) {
			/* Do not let a completed listing repaint/reset a scrolling list.
			 * Keep ownership on the worker until the UI is quiet again. */
			if (!ppa_browser_input_settled()) {
				ppa_browser_directory_worker_unlock();
				return 0;
			}
			*out = g_ppa_browser_directory_worker.result;
			memset(&g_ppa_browser_directory_worker.result, 0,
			       sizeof(g_ppa_browser_directory_worker.result));
			g_ppa_browser_directory_worker.result_ready = 0;
			available = 1;
		}
		else if (g_ppa_browser_directory_worker.result_ready) {
			discard = g_ppa_browser_directory_worker.result.items;
			memset(&g_ppa_browser_directory_worker.result, 0,
			       sizeof(g_ppa_browser_directory_worker.result));
			g_ppa_browser_directory_worker.result_ready = 0;
		}
		ppa_browser_directory_worker_unlock();
	}
	if (discard != 0)
		free(discard);
	return available;
}

static size_t ppa_browser_movie_stem_length(const char *name)
{
	const char *dot;
	if (name == 0)
		return 0;
	dot = strrchr(name, '.');
	if (dot == 0 || dot == name)
		return 0;
	return (size_t)(dot - name);
}

static int ppa_browser_attachment_matches(const char *movie_name,
                                          const char *attachment_name,
                                          const char *extension,
                                          size_t extension_length)
{
	size_t stem_length = ppa_browser_movie_stem_length(movie_name);
	size_t attachment_length;
	if (stem_length == 0 || attachment_name == 0 || extension == 0 ||
	    extension_length == 0)
		return 0;
	attachment_length = strlen(attachment_name);
	if (attachment_length != stem_length + extension_length)
		return 0;
	return strnicmp(movie_name, attachment_name, stem_length) == 0 &&
	       strnicmp(attachment_name + stem_length, extension,
	                extension_length) == 0;
}

static void ppa_browser_build_item_path(char *out,
                                        size_t out_size,
                                        const char *file_path,
                                        const char *file_short_path,
                                        const directory_item_struct *item)
{
	const char *base;
	const char *name;

	if (out == 0 || out_size == 0)
		return;

	out[0] = 0;

	if (item == 0)
		return;

	if (file_path != 0 && ppa_vfs_is_ntfs_path(file_path)) {
		base = file_path;
		name = (item->compname != 0 && item->compname[0] != 0) ?
		       item->compname : item->longname;
	}
	else {
		base = (file_short_path != 0) ? file_short_path : "";
		name = item->shortname;
	}

	if (name == 0)
		name = "";

	/* A truncated path can name a different file. Reject it instead of
	 * opening, probing or deleting an unintended prefix. */
	int written = snprintf(out, out_size, "%s%s", base ? base : "", name);
	if (written < 0 || (size_t)written >= out_size)
		out[0] = 0;
}

static void ppa_browser_pop_path(char *path,
                                 char *removed,
                                 size_t removed_size)
{
	int end;
	char *slash;
	if (removed != 0 && removed_size != 0) removed[0] = 0;
	if (path == 0 || path[0] == 0) return;
	end = (int)strlen(path) - 1;
	while (end >= 0 && path[end] == '/') path[end--] = 0;
	slash = strrchr(path, '/');
	if (slash != 0) {
		if (removed != 0 && removed_size != 0) {
			strncpy(removed, slash + 1, removed_size - 1U);
			removed[removed_size - 1U] = 0;
		}
		slash[1] = 0;
	}
	else {
		if (removed != 0 && removed_size != 0) {
			strncpy(removed, path, removed_size - 1U);
			removed[removed_size - 1U] = 0;
		}
		path[0] = 0;
	}
}

static volatile int g_ppa_browser_resume_recovery_pending = 0;
static volatile int g_ppa_browser_metadata_suppressed = 0;

typedef struct ppa_browser_film_metadata {
	unsigned int generation;
	int loaded;
	int ok;
	int item_index;
	int filetype;
	char item_key[256];
	u32 total_frames;
	u32 width;
	u32 height;
	u32 scale;
	u32 rate;
	u32 audio_streams;
	u32 subtitles;
} ppa_browser_film_metadata;

typedef struct ppa_browser_metadata_task_struct {
	unsigned int generation;
	int item_index;
	int filetype;
	char item_key[256];
	char path[512];
} ppa_browser_metadata_task;

typedef struct ppa_browser_metadata_worker_struct {
	SceUID lock;
	SceUID wake;
	SceUID thread;
	volatile int initialized;
	volatile int stop;
	volatile unsigned int generation;
	volatile unsigned int active_generation;
	int pending;
	int running;
	int result_ready;
	ppa_browser_metadata_task task;
	ppa_browser_film_metadata result;
} ppa_browser_metadata_worker;

typedef struct ppa_browser_metadata_selection_struct {
	int valid;
	int scheduled;
	int result_valid;
	int item_index;
	int filetype;
	u64 due_us;
	char item_key[256];
	char path[512];
	ppa_browser_film_metadata result;
} ppa_browser_metadata_selection;

static ppa_browser_metadata_worker g_ppa_browser_metadata_worker;
static ppa_browser_metadata_selection g_ppa_browser_metadata_selection;

static void ppa_browser_metadata_worker_cancel(int wait_for_idle);
static void ppa_browser_metadata_worker_shutdown(void);

static int ppa_browser_is_movie_filetype(int filetype)
{
	return ppa::media::isSupportedFileType((file_type_enum)filetype) ? 1 : 0;
}

static void ppa_browser_metadata_set_defaults(ppa_browser_film_metadata *m)
{
	if (m == 0)
		return;
	memset(m, 0, sizeof(*m));
	m->loaded = 1;
	m->item_index = -1;
	m->filetype = FS_UNKNOWN_FILE;
	m->scale = 1;
	m->rate = 1;
}

static int ppa_browser_metadata_io_allowed(void)
{
	return !g_ppa_browser_metadata_suppressed;
}

static int ppa_browser_metadata_defer_one_paint_tick(void)
{
	return !ppa_browser_metadata_io_allowed();
}

static void ppa_browser_metadata_make_item_key(char *out,
                                               size_t out_size,
                                               const directory_item_struct *item)
{
	const char *name;
	if (out == 0 || out_size == 0)
		return;
	out[0] = 0;
	if (item == 0)
		return;
	name = (item->compname != 0 && item->compname[0] != 0) ?
	       item->compname : item->longname;
	if (name == 0)
		name = "";
	snprintf(out, out_size, "%s", name);
	out[out_size - 1] = 0;
}

static int ppa_browser_load_metadata(int filetype,
                                     const char *path,
                                     ppa_browser_film_metadata *m)
{
	const ppa::media::Backend *backend;
	ppa::media::Metadata metadata;
	if (path == 0 || m == 0)
		return 0;
	backend = ppa::media::backendForFileType((file_type_enum)filetype);
	if (backend == 0 || !backend->probe(path, metadata))
		return 0;
	m->total_frames = metadata.totalFrames;
	m->width = metadata.width;
	m->height = metadata.height;
	m->scale = metadata.scale ? metadata.scale : 1;
	m->rate = metadata.rate ? metadata.rate : 1;
	m->audio_streams = metadata.audioStreams;
	m->subtitles = metadata.embeddedSubtitles;
	m->ok = metadata.valid ? 1 : 0;
	return m->ok;
}

static int ppa_browser_metadata_worker_lock(void)
{
	return g_ppa_browser_metadata_worker.initialized &&
	       sceKernelWaitSema(g_ppa_browser_metadata_worker.lock, 1, 0) >= 0;
}

static int ppa_browser_metadata_worker_try_lock(void)
{
	return g_ppa_browser_metadata_worker.initialized &&
	       sceKernelPollSema(g_ppa_browser_metadata_worker.lock, 1) >= 0;
}

static void ppa_browser_metadata_worker_unlock(void)
{
	if (g_ppa_browser_metadata_worker.initialized)
		sceKernelSignalSema(g_ppa_browser_metadata_worker.lock, 1);
}

static int ppa_browser_metadata_worker_cancel_check(void *user)
{
	ppa_browser_metadata_worker *worker =
		(ppa_browser_metadata_worker *)user;
	if (worker == 0)
		return 1;
	for (;;) {
		if (worker->stop || __sync_fetch_and_add(&worker->generation, 0U) !=
		                    worker->active_generation)
			return 1;
		/* Observe the sampler directly, before the UI has time to process the
		 * edge and advance the generation. Never continue probing through a
		 * slow repaint with a D-pad press already waiting in the queue. */
		if (!ppa_browser_input_settled())
			return 1;
		if (!ppa_browser_idle_io_expired())
			return 0;
		sceKernelDelayThread(50000U);
	}
}

static int ppa_browser_metadata_worker_thread(SceSize args, void *argp)
{
	ppa_browser_metadata_worker *worker = &g_ppa_browser_metadata_worker;
	(void)args;
	(void)argp;

	while (worker != 0) {
		ppa_browser_metadata_task task;
		ppa_browser_film_metadata result;
		int acquired = 0;
		int cancelled = 0;

		if (sceKernelWaitSema(worker->wake, 1, 0) < 0)
			break;
		if (!ppa_browser_metadata_worker_lock())
			break;
		if (worker->stop) {
			ppa_browser_metadata_worker_unlock();
			break;
		}
		if (!worker->pending) {
			ppa_browser_metadata_worker_unlock();
			continue;
		}
		task = worker->task;
		worker->pending = 0;
		worker->running = 1;
		worker->active_generation = task.generation;
		ppa_browser_metadata_worker_unlock();

		ppa_browser_metadata_set_defaults(&result);
		result.generation = task.generation;
		result.item_index = task.item_index;
		result.filetype = task.filetype;
		strncpy(result.item_key, task.item_key,
		        sizeof(result.item_key) - 1U);

		/* The directory reader and metadata parser share a cancellable
		 * filesystem gate. The UI never waits for this gate. */
		acquired = ppa_io_background_acquire(
			ppa_browser_metadata_worker_cancel_check, worker);
		if (acquired) {
			/* Container parsing uses libbufferedio's own cancellation slots;
			 * NTFS/FAT backend calls additionally use the common I/O slots. */
			io_set_cancel_check(ppa_browser_metadata_worker_cancel_check, worker);
			ppa_io_set_cancel_check(ppa_browser_metadata_worker_cancel_check, worker);
			(void)ppa_browser_load_metadata(task.filetype, task.path, &result);
			cancelled = ppa_browser_metadata_worker_cancel_check(worker);
			if (cancelled) {
				result.ok = 0;
				result.total_frames = result.width = result.height = 0;
				result.audio_streams = result.subtitles = 0;
			}
			io_set_cancel_check(0, 0);
			ppa_io_set_cancel_check(0, 0);
			ppa_io_background_release();
		}

		if (!ppa_browser_metadata_worker_lock())
			break;
		worker->running = 0;
		if (acquired && !cancelled && !worker->stop &&
		    __sync_fetch_and_add(&worker->generation, 0U) == task.generation) {
			worker->result = result;
			worker->result_ready = 1;
		}
		ppa_browser_metadata_worker_unlock();
	}
	io_set_cancel_check(0, 0);
	ppa_io_set_cancel_check(0, 0);
	return 0;
}

static int ppa_browser_metadata_worker_init(void)
{
	ppa_browser_metadata_worker *worker = &g_ppa_browser_metadata_worker;
	if (worker->initialized)
		return 1;
	memset(worker, 0, sizeof(*worker));
	worker->lock = worker->wake = worker->thread = -1;
	worker->lock = sceKernelCreateSema("ppa_browser_meta_lock", 0, 1, 1, 0);
	worker->wake = sceKernelCreateSema("ppa_browser_meta_wake", 0, 0, 1, 0);
	if (worker->lock < 0 || worker->wake < 0)
		goto fail;
	worker->initialized = 1;
	worker->generation = 1;
	worker->thread = ppa_thread_create(PPA_THREAD_BROWSER_METADATA,
	                                   "ppa_browser_metadata",
	                                   ppa_browser_metadata_worker_thread,
	                                   64 * 1024, 0);
	if (worker->thread < 0 ||
	    sceKernelStartThread(worker->thread, 0, 0) < 0)
		goto fail;
	return 1;

fail:
	if (worker->thread >= 0) sceKernelDeleteThread(worker->thread);
	if (worker->wake >= 0) sceKernelDeleteSema(worker->wake);
	if (worker->lock >= 0) sceKernelDeleteSema(worker->lock);
	memset(worker, 0, sizeof(*worker));
	worker->lock = worker->wake = worker->thread = -1;
	return 0;
}

static void ppa_browser_metadata_worker_cancel(int wait_for_idle)
{
	ppa_browser_metadata_worker *worker = &g_ppa_browser_metadata_worker;
	int running;
	if (!worker->initialized)
		return;
	/* Cancellation is a single atomic publication, independent of the
	 * worker semaphore. A syscall already in progress finishes on its owner;
	 * no subsequent parser read or stale result is accepted. */
	(void)ppa_browser_cancel_generation(&worker->generation);
	if (wait_for_idle ? ppa_browser_metadata_worker_lock() :
	                    ppa_browser_metadata_worker_try_lock()) {
		worker->pending = 0;
		worker->result_ready = 0;
		ppa_browser_metadata_worker_unlock();
	}
	if (!wait_for_idle)
		return;
	do {
		running = 0;
		if (ppa_browser_metadata_worker_lock()) {
			running = worker->running;
			ppa_browser_metadata_worker_unlock();
		}
		if (running) sceKernelDelayThread(1000);
	} while (running);
}

static void ppa_browser_metadata_worker_shutdown(void)
{
	ppa_browser_metadata_worker *worker = &g_ppa_browser_metadata_worker;
	if (!worker->initialized)
		return;
	ppa_browser_metadata_worker_cancel(1);
	if (ppa_browser_metadata_worker_lock()) {
		worker->stop = 1;
		ppa_browser_metadata_worker_unlock();
	}
	sceKernelSignalSema(worker->wake, 1);
	if (worker->thread >= 0) {
		sceKernelWaitThreadEnd(worker->thread, 0);
		sceKernelDeleteThread(worker->thread);
	}
	if (worker->wake >= 0) sceKernelDeleteSema(worker->wake);
	if (worker->lock >= 0) sceKernelDeleteSema(worker->lock);
	memset(worker, 0, sizeof(*worker));
	worker->lock = worker->wake = worker->thread = -1;
}

static int ppa_browser_metadata_worker_schedule(const char *file_path,
                                                const char *file_short_path,
                                                const directory_item_struct *item,
                                                int item_index)
{
	ppa_browser_metadata_worker *worker = &g_ppa_browser_metadata_worker;
	ppa_browser_metadata_task task;
	int accepted = 0;
	if (item == 0 || !ppa_browser_is_movie_filetype(item->filetype) ||
	    !ppa_browser_metadata_worker_init())
		return 0;
	memset(&task, 0, sizeof(task));
	task.item_index = item_index;
	task.filetype = item->filetype;
	ppa_browser_metadata_make_item_key(task.item_key,
	                                  sizeof(task.item_key), item);
	ppa_browser_build_item_path(task.path, sizeof(task.path),
	                            file_path, file_short_path, item);
	if (task.path[0] == 0)
		return 0;
	if (ppa_browser_metadata_worker_try_lock()) {
		if (!worker->stop && !worker->running && !worker->pending) {
			task.generation = __sync_fetch_and_add(&worker->generation, 0U);
			worker->task = task;
			worker->pending = 1;
			accepted = 1;
		}
		ppa_browser_metadata_worker_unlock();
	}
	if (accepted)
		sceKernelSignalSema(worker->wake, 1);
	return accepted;
}

static void ppa_browser_metadata_clear_cache(void)
{
	ppa_browser_metadata_worker_cancel(0);
	memset(&g_ppa_browser_metadata_selection, 0,
	       sizeof(g_ppa_browser_metadata_selection));
	g_ppa_browser_metadata_selection.item_index = -1;
	ppa_browser_metadata_set_defaults(
		&g_ppa_browser_metadata_selection.result);
}

static void ppa_browser_metadata_input_activity(void)
{
	ppa_browser_metadata_selection *selection =
		&g_ppa_browser_metadata_selection;
	/* Publish cancellation before reopening the idle window. Otherwise an
	 * old selection parked at the idle cutoff can resume card reads in front
	 * of the key event that will replace it. Completed results remain cached. */
	if (selection->scheduled) {
		ppa_browser_metadata_worker_cancel(0);
		selection->scheduled = 0;
	}
	selection->due_us = sceKernelGetSystemTimeWide() +
	                    PPA_BROWSER_METADATA_DWELL_US;
	ppa_browser_note_activity();
}

static void ppa_browser_metadata_schedule_wake_cooldown(void)
{
	/* A fresh two-second dwell begins after playback/resume; no old result is
	 * retained and no parser opens merely because the menu was repainted. */
	ppa_browser_note_activity();
	ppa_browser_metadata_clear_cache();
}

static void ppa_browser_metadata_cache_prepare(const char *file_path,
                                               const char *file_short_path,
                                               directory_item_struct *items,
                                               int item_count,
                                               int requested_index)
{
	char key[256];
	char path[512];
	ppa_browser_metadata_selection *selection =
		&g_ppa_browser_metadata_selection;
	if (!ppa_browser_metadata_io_allowed() || items == 0 || item_count <= 0 ||
	    requested_index < 0 || requested_index >= item_count ||
	    !ppa_browser_is_movie_filetype(items[requested_index].filetype)) {
		ppa_browser_metadata_clear_cache();
		return;
	}
	ppa_browser_metadata_make_item_key(key, sizeof(key),
	                                  &items[requested_index]);
	ppa_browser_build_item_path(path, sizeof(path), file_path, file_short_path,
	                            &items[requested_index]);
	if (selection->valid && selection->item_index == requested_index &&
	    selection->filetype == items[requested_index].filetype &&
	    strcmp(selection->item_key, key) == 0 &&
	    strcmp(selection->path, path) == 0)
		return;

	ppa_browser_metadata_clear_cache();
	selection->valid = 1;
	selection->item_index = requested_index;
	selection->filetype = items[requested_index].filetype;
	selection->due_us = sceKernelGetSystemTimeWide() +
	                    PPA_BROWSER_METADATA_DWELL_US;
	strncpy(selection->item_key, key, sizeof(selection->item_key) - 1U);
	strncpy(selection->path, path, sizeof(selection->path) - 1U);
}

static int ppa_browser_metadata_worker_commit_result(
		directory_item_struct *items, int item_count)
{
	ppa_browser_film_metadata result;
	int have_result = 0;
	ppa_browser_metadata_selection *selection =
		&g_ppa_browser_metadata_selection;
	if (!g_ppa_browser_metadata_worker.initialized)
		return 0;
	if (ppa_browser_metadata_worker_try_lock()) {
		if (g_ppa_browser_metadata_worker.result_ready) {
			result = g_ppa_browser_metadata_worker.result;
			g_ppa_browser_metadata_worker.result_ready = 0;
			have_result = 1;
		}
		ppa_browser_metadata_worker_unlock();
	}
	if (!have_result || !selection->valid || items == 0 ||
	    result.generation !=
	        __sync_fetch_and_add(&g_ppa_browser_metadata_worker.generation, 0U) ||
	    result.filetype != selection->filetype ||
	    result.item_index != selection->item_index ||
	    result.item_index < 0 || result.item_index >= item_count ||
	    strcmp(result.item_key, selection->item_key) != 0)
		return 0;
	selection->result = result;
	selection->result_valid = 1;
	selection->scheduled = 0;
	return 1;
}

static int ppa_browser_metadata_cache_get(const char *file_path,
                                          const char *file_short_path,
                                          int show_hidden,
                                          int show_unknown,
                                          directory_item_struct *items,
                                          int item_count,
                                          int item_index,
                                          ppa_browser_film_metadata *out)
{
	ppa_browser_metadata_selection *selection =
		&g_ppa_browser_metadata_selection;
	(void)file_path;
	(void)file_short_path;
	(void)show_hidden;
	(void)show_unknown;
	(void)ppa_browser_metadata_worker_commit_result(items, item_count);
	if (!selection->valid || !selection->result_valid ||
	    item_index != selection->item_index || items == 0 ||
	    item_index < 0 || item_index >= item_count)
		return 0;
	if (out != 0)
		*out = selection->result;
	return 1;
}

static int ppa_browser_metadata_cache_idle_pump(const char *file_path,
                                                const char *file_short_path,
                                                int show_hidden,
                                                int show_unknown,
                                                directory_item_struct *items,
                                                int item_count,
                                                int requested_index)
{
	ppa_browser_metadata_selection *selection =
		&g_ppa_browser_metadata_selection;
	if (!ppa_browser_metadata_io_allowed() || items == 0 || item_count <= 0 ||
	    requested_index < 0 || requested_index >= item_count)
		return 0;
	ppa_browser_metadata_cache_prepare(file_path, file_short_path,
	                                   items, item_count, requested_index);
	if (ppa_browser_metadata_worker_commit_result(items, item_count))
		return 1;
	if (ppa_browser_idle_io_expired() || !selection->valid ||
	    selection->result_valid || selection->scheduled ||
	    sceKernelGetSystemTimeWide() < selection->due_us ||
	    !ppa_browser_input_settled())
		return 0;
	if (ppa_browser_metadata_worker_schedule(file_path, file_short_path,
	                                        &items[requested_index],
	                                        requested_index)) {
		selection->scheduled = 1;
	}
	return 0;
}

static char *ppa_play_movie_dispatch(int filetype,
	struct movie_file_struct *movie,
	int usePos,
	int pspType,
	int tvAspectRatio,
	int tvOverScanLeft,
	int tvOverScanTop,
	int tvOverScanRight,
	int tvOverScanBottom,
	int videoMode, UiI18n *ui)
{
	const ppa::media::Backend *backend =
		ppa::media::backendForFileType((file_type_enum)filetype);
	ppa::media::PlaybackRequest request;
	char *result;
	int configured_auto_clock = 1;
	struct ppa_session_options session_options = {0, 0, 0, 1};

	if (backend == 0 || movie == 0)
		return const_cast<char *>("unsupported movie container");

	{
		Config *config = Config::getInstance();
		if (config != 0) {
			session_options.audio_only = config->getBooleanValue("config/player/audio_only", false);
			session_options.health_panel = config->getBooleanValue("config/player/playback_health", false);
			session_options.sleep_minutes = config->getIntegerValue("config/player/sleep_timer_minutes", 0);
			configured_auto_clock =
				config->getBooleanValue("config/cpu/auto_clock", true) ? 1 : 0;
			subtitle_preferences_set_preferred_language(
				config->getStringValue("config/subtitles/preferred_language", "en"));
		}
	}

	ppa_session_configure(&session_options);
	int uiReady = ppa_playback_ui_prepare(ui);
	/* Localized Health preparation can reopen a Latin fallback after the
	 * browser release. Playback consumes baked textures only; reclaim those
	 * temporary faces on both success and failure before decoder allocation. */
	if (ui != NULL && ui->getFont() != NULL)
		ui->getFont()->releaseFallbackFonts();
	if (!uiReady)
		return const_cast<char *>("playback: UI notice allocation failed");
	request.movie = movie;
	request.resume = usePos != 0;
	request.pspType = pspType;
	request.tvAspectRatio = tvAspectRatio;
	request.overscanLeft = tvOverScanLeft;
	request.overscanTop = tvOverScanTop;
	request.overscanRight = tvOverScanRight;
	request.overscanBottom = tvOverScanBottom;
	request.videoMode = videoMode;

	ppa_video_pipeline_reload_config();
	/* Re-apply the user's auto-clock preference before entering the forced-on
	 * battery-saver governor. This also handles toggling battery mode between
	 * consecutive playback sessions without preserving a stale preference. */
	cpu_clock_set_extreme_battery_saver(0);
	cpu_clock_set_auto_enabled(configured_auto_clock);
	cpu_clock_set_extreme_battery_saver(
		ppa_video_pipeline_extreme_battery_saver_enabled());
	ppa_video_pipeline_reset();
	cpu_clock_enter_playback();

	/* Browser launch/exit transitions must not leak into playback or back into
	 * the browser. Held state remains tracked by the sampler. */
	ctrl_flush();
	result = backend->play(request);
	ppa_playback_ui_release();
	ctrl_flush();

	cpu_clock_auto_finish_session();
	ppa_video_pipeline_leave_playback();
	cpu_clock_leave_playback();
	return result;
}

PpuPlayer::PpuPlayer() {
	drawImage = NULL;
	uiI18n = NULL;
	browserTextCache = new (std::nothrow) BrowserTextCache();
	startupCredit = NULL;
	startupCreditVisibleSinceUs = 0;
	startupCreditFadeStartedUs = 0;
	startupCreditOpacity = 0;
	isSuspended = false;

	filmPreviewImage = NULL;
	filmPreviewUnavailable = true;

	fileItems = NULL;

	attachmentItems = NULL;

	batteryPercent10 = NULL;
	batteryPercent33 = NULL;
	batteryPercent66 = NULL;
	batteryPercent100 = NULL;
	batteryCharging = NULL;

	filesystemMode = 0;
	powerEventFlags = 0;

};

PpuPlayer::~PpuPlayer() {
	releaseStartupCredit();
	delete browserTextCache;
	browserTextCache = NULL;

	ppa_browser_directory_worker_shutdown();
	ppa_browser_metadata_worker_shutdown();
	ppa_browser_fs_cache_shutdown();
	ppa_io_background_shutdown();

	if ( fileItems ) {
		free(fileItems);
	}

	if ( attachmentItems ) {
		free(attachmentItems);
	}

	if ( drawImage ) {
		freeImage(drawImage);
		drawImage = NULL;
	}

	if ( filmPreviewImage ) {
		freeImage(filmPreviewImage);
		filmPreviewImage = NULL;
	}

	if ( batteryPercent10 ) {
		freeImage(batteryPercent10);
		batteryPercent10 = NULL;
	}

	if ( batteryPercent33 ) {
		freeImage(batteryPercent33);
		batteryPercent33 = NULL;
	}

	if ( batteryPercent66 ) {
		freeImage(batteryPercent66);
		batteryPercent66 = NULL;
	}

	if ( batteryPercent100 ) {
		freeImage(batteryPercent100);
		batteryPercent100 = NULL;
	}

	if ( batteryCharging ) {
		freeImage(batteryCharging);
		batteryCharging = NULL;
	}

	if ( uiI18n ) {
		delete uiI18n;
		uiI18n = NULL;
	}

	ctrl_destroy();
	ppa_vfs_shutdown();
	fat_free();
	disableGraphics();
	sceDisplayWaitVblankStart();
	sceGuTerm();

	FtFontManager::freeFtFontManager();
	Skin::freeSkin();
	Config::freeConfig();

	gu_font_close();

};

void PpuPlayer::initSkinData() {
	if (browserTextCache != NULL) browserTextCache->clear();
	Skin* skin = NULL;
	skin = Skin::getInstance();

	char tempPath[1024];

	/* File-list palette and viewport geometry. */
	fileListTextColor = skin->getColorValue("skin/font_color/color", 0xFFFFFF);
	fileListHLTextColor = skin->getColorValue("skin/files_list/highlight_color", 0xFFFFFF);
	fileListHLBackgroundColor = skin->getColorValue("skin/files_list/highlight_background_color", 0x000000);

	if ( skin->getBooleanValue("skin/files_list/highlight_alpha_enable", false) ) {
		Color alpha = skin->getColorValue("skin/files_list/highlight_alpha_value", 0xFF);
		fileListHLBackgroundColor = (alpha << 24) | fileListHLBackgroundColor;
	}
	else {
		fileListHLBackgroundColor = 0xff000000 | fileListHLBackgroundColor;
	}
	/* The legacy default skin used identical normal/highlight text colors and
	 * a very low-alpha black fill, which made D-pad movement look completely
	 * inert on the textless skin.  A narrow, high-contrast cursor rail keeps
	 * selection movement unambiguous without covering filenames. */
	fileListCursorColor = skin->getColorValue("skin/files_list/cursor_color", 0x66DDE5);
	fileListScrollbarColor = 0xC0000000 |
	                         (skin->getColorValue("skin/files_list/scrollbar_color",
	                                              fileListCursorColor) & 0x00FFFFFF);
	fileListCursorWidth = skin->getIntegerValue("skin/files_list/cursor_width", 3);
	if (fileListCursorWidth < 2)
		fileListCursorWidth = 2;
	if (fileListCursorWidth > 6)
		fileListCursorWidth = 6;

	fileListBoxLeft = skin->getIntegerValue("skin/files_list/left", 0);
	fileListBoxTop = skin->getIntegerValue("skin/files_list/top", 0);
	fileListBoxWidth = skin->getIntegerValue("skin/files_list/width", 480);
	fileListBoxHeight = skin->getIntegerValue("skin/files_list/height", 272);

	fileItemBottom = fileListBoxHeight / ( textPixelSize + 2*TEXT_ITEM_BORDER );
	if (fileItemBottom < 1)
		fileItemBottom = 1;

	/* Optional date/time skin panel. */
	dateVisible = skin->getBooleanValue("skin/datetime_pannel/date_label/visible", false);
	dateLeft = skin->getIntegerValue("skin/datetime_pannel/date_label/left", 0);
	dateTop = skin->getIntegerValue("skin/datetime_pannel/date_label/top", 0);
	dateWidth = skin->getIntegerValue("skin/datetime_pannel/date_label/width", 144);
	dateHeight = skin->getIntegerValue("skin/datetime_pannel/date_label/height", 80);
	timeVisible = skin->getBooleanValue("skin/datetime_pannel/time_label/visible", false);
	timeLeft = skin->getIntegerValue("skin/datetime_pannel/time_label/left", 0);
	timeTop = skin->getIntegerValue("skin/datetime_pannel/time_label/top", 0);
	timeWidth = skin->getIntegerValue("skin/datetime_pannel/time_label/width", 144);
	timeHeight = skin->getIntegerValue("skin/datetime_pannel/time_label/height", 80);

	/* Battery labels and icon assets. */
	batteryStatusVisible = skin->getBooleanValue("skin/battery_pannel/battery_status_label/visible", false);
	batteryStatusLeft = skin->getIntegerValue("skin/battery_pannel/battery_status_label/left", 0);
	batteryStatusTop = skin->getIntegerValue("skin/battery_pannel/battery_status_label/top", 0);
	batteryStatusWidth = skin->getIntegerValue("skin/battery_pannel/battery_status_label/width", 144);
	batteryStatusHeight = skin->getIntegerValue("skin/battery_pannel/battery_status_label/height", 80);
	batteryLifeVisible = skin->getBooleanValue("skin/battery_pannel/battery_life_label/visible", false);
	batteryLifeLeft = skin->getIntegerValue("skin/battery_pannel/battery_life_label/left", 0);
	batteryLifeTop = skin->getIntegerValue("skin/battery_pannel/battery_life_label/top", 0);
	batteryLifeWidth = skin->getIntegerValue("skin/battery_pannel/battery_life_label/width", 144);
	batteryLifeHeight = skin->getIntegerValue("skin/battery_pannel/battery_life_label/height", 80);

	memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", skinPath, skin->getStringValue("skin/battery_pannel/battery_status_image/percent10", "10.png"));
	freeImage(batteryPercent10);
	batteryPercent10 = loadPNGImage(tempPath);
	memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", skinPath, skin->getStringValue("skin/battery_pannel/battery_status_image/percent33", "33.png"));
	freeImage(batteryPercent33);
	batteryPercent33 = loadPNGImage(tempPath);
	memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", skinPath, skin->getStringValue("skin/battery_pannel/battery_status_image/percent66", "66.png"));
	freeImage(batteryPercent66);
	batteryPercent66 = loadPNGImage(tempPath);
	memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", skinPath, skin->getStringValue("skin/battery_pannel/battery_status_image/percent100", "100.png"));
	freeImage(batteryPercent100);
	batteryPercent100 = loadPNGImage(tempPath);
	memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", skinPath, skin->getStringValue("skin/battery_pannel/battery_status_image/charging", "charging.png"));
	batteryCharging = loadPNGImage(tempPath);

	filmReloadEnable = true;

	/* Selected-file preview panel. */
#if PPA_ENABLE_THUMBNAILS
	filmPreviewVisible = skin->getBooleanValue("skin/film_preview_pannel/preview_image/visible", false);
	filmPreviewReload = filmReloadEnable;
	filmPreviewLeft = skin->getIntegerValue("skin/film_preview_pannel/preview_image/left", 0);
	filmPreviewTop = skin->getIntegerValue("skin/film_preview_pannel/preview_image/top", 0);
	filmPreviewWidth = skin->getIntegerValue("skin/film_preview_pannel/preview_image/width", 144);
	filmPreviewHeight = skin->getIntegerValue("skin/film_preview_pannel/preview_image/height", 80);
#else
	filmPreviewVisible = false;
	filmPreviewReload = false;
	filmPreviewLeft = filmPreviewTop = 0;
	filmPreviewWidth = filmPreviewHeight = 0;
#endif

	/* Selected-file metadata panel. */
	filmInformationReload = filmReloadEnable;
	filmAspectRatioVisible = skin->getBooleanValue("skin/film_information_pannel/aspect_ratio_label/visible", false);
	filmAspectRatioLeft = skin->getIntegerValue("skin/film_information_pannel/aspect_ratio_label/left", 0);
	filmAspectRatioTop = skin->getIntegerValue("skin/film_information_pannel/aspect_ratio_label/top", 0);
	filmAspectRatioWidth = skin->getIntegerValue("skin/film_information_pannel/aspect_ratio_label/width", 144);
	filmAspectRatioHeight = skin->getIntegerValue("skin/film_information_pannel/aspect_ratio_label/height", 80);

	filmFpsVisible = skin->getBooleanValue("skin/film_information_pannel/fps_label/visible", false);
	filmFpsLeft = skin->getIntegerValue("skin/film_information_pannel/fps_label/left", 0);
	filmFpsTop = skin->getIntegerValue("skin/film_information_pannel/fps_label/top", 0);
	filmFpsWidth = skin->getIntegerValue("skin/film_information_pannel/fps_label/width", 144);
	filmFpsHeight = skin->getIntegerValue("skin/film_information_pannel/fps_label/height", 80);

	filmTotalTimeVisible = skin->getBooleanValue("skin/film_information_pannel/total_time_label/visible", false);
	filmTotalTimeLeft = skin->getIntegerValue("skin/film_information_pannel/total_time_label/left", 0);
	filmTotalTimeTop = skin->getIntegerValue("skin/film_information_pannel/total_time_label/top", 0);
	filmTotalTimeWidth = skin->getIntegerValue("skin/film_information_pannel/total_time_label/width", 144);
	filmTotalTimeHeight = skin->getIntegerValue("skin/film_information_pannel/total_time_label/height", 80);

	filmAudioStreamsVisible = skin->getBooleanValue("skin/film_information_pannel/audio_streams_label/visible", false);
	filmAudioStreamsLeft = skin->getIntegerValue("skin/film_information_pannel/audio_streams_label/left", 0);
	filmAudioStreamsTop = skin->getIntegerValue("skin/film_information_pannel/audio_streams_label/top", 0);
	filmAudioStreamsWidth = skin->getIntegerValue("skin/film_information_pannel/audio_streams_label/width", 144);
	filmAudioStreamsHeight = skin->getIntegerValue("skin/film_information_pannel/audio_streams_label/height", 80);

	filmSubtitlesVisible = skin->getBooleanValue("skin/film_information_pannel/subtitles_label/visible", false);
	filmSubtitlesLeft = skin->getIntegerValue("skin/film_information_pannel/subtitles_label/left", 0);
	filmSubtitlesTop = skin->getIntegerValue("skin/film_information_pannel/subtitles_label/top", 0);
	filmSubtitlesWidth = skin->getIntegerValue("skin/film_information_pannel/subtitles_label/width", 144);
	filmSubtitlesHeight = skin->getIntegerValue("skin/film_information_pannel/subtitles_label/height", 80);

	if (uiI18n == NULL)
		uiI18n = new (std::nothrow) UiI18n();
	if (uiI18n != NULL && !uiI18n->load(applicationPath, skinPath)) {
	}

	/* Clip 15 is the authoritative text viewport for textless skins. It is
	 * intentionally applied after JSON selection so list rows, highlight, and
	 * scrollbar all share the exact reduced pane geometry. */
	if (uiI18n != NULL && uiI18n->isReady()) {
		int clipLeft, clipTop, clipWidth, clipHeight;
		if (uiI18n->getMainClipRect("main_directory_list_area",
		                              &clipLeft, &clipTop,
		                              &clipWidth, &clipHeight)) {
			fileListBoxLeft = clipLeft;
			fileListBoxTop = clipTop;
			fileListBoxWidth = clipWidth;
			fileListBoxHeight = clipHeight;
			fileItemBottom = fileListBoxHeight /
			                 (textPixelSize + 2 * TEXT_ITEM_BORDER);
			if (fileItemBottom < 1) fileItemBottom = 1;
		}
	}

};

int PpuPlayer::init(char* ppaPath) {

	char tempPath[1024];
	SceUID modid;
	int fixed_surfaces_reserved = 0;

	pspType = ppa_hardware_model();

	if (ppaPath == NULL) {
		if (getcwd(tempPath, sizeof(tempPath)) == NULL)
			return 0;
		ppaPath = tempPath;
	}
	{
		int path_length = snprintf(applicationPath, sizeof(applicationPath),
		                           "%s/", ppaPath);
		if (path_length < 0 || path_length >= (int)sizeof(applicationPath)) {
			return 0;
		}
	}

	/* Start the high-priority sampler before PRX/font/skin/storage bootstrap.
	 * Short presses are captured even while boot performs unavoidable I/O. */
	if (!ctrl_init())
		return 0;

	memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", applicationPath, "cooleyesBridge.prx");
	if (!ppa_boot_required_module(tempPath, &modid)) {
		ppa_boot_fail_screen("Could not load/start required cooleyesBridge.prx");
		return 0;
	}
	if (!ppa_privileged_bridge_attach()) {
		ppa_boot_fail_screen("cooleyesBridge PRX/API mismatch");
		return 0;
	}

    memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", applicationPath, "miniconv.prx");
	if (!ppa_boot_required_module(tempPath, &modid))
		return 0;

    memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", applicationPath, "i18n.prx");
	if (!ppa_boot_required_module(tempPath, &modid))
		return 0;

	memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", applicationPath, "dvemgr.prx");
	if ( ! VideoMode::init(pspType, tempPath))
		return 0;

	/* Initial bridge/UI/TV-out PRXs are resident. Reserve the legacy
	 * Sony fixed-address surfaces before AVC buffer setup, but retain a
	 * complete PSP-1000-style fallback when CFW refuses the address. */
	fixed_surfaces_reserved = ppa_memory_prepare_fixed_surfaces();

	if( mp4_avc_init_ddrtop() != 1 )
		return 0;

	if (!m33IsTVOutSupported(pspType) || !fixed_surfaces_reserved)
		if( psp1k_init_frame_buffer() != 1 )
			return 0;

	memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", applicationPath, "moviestat.dat");
	init_movie_stat(tempPath);

	memset(fontPath, 0, 1024);
	snprintf(fontPath, sizeof(fontPath), "%s%s", applicationPath, "fonts/");
	subtitle_font_config_set_directory(fontPath);

	memset(skinPath, 0, 1024);
	snprintf(skinPath, sizeof(skinPath), "%s%s", applicationPath, "skins/");
	drawImage = createImage(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	if (!drawImage)
		return 0;

	Config* config = NULL;
	FtFontManager* fontManager = NULL;
	FtFont* mainFont = NULL;

	/* Runtime configuration must be available before any skin-owned assets. */
	memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", applicationPath, "config.xml");

	if (!Config::loadConfig(tempPath)) {
		ppa_boot_fail_screen("Could not load config.xml");
		return 0;
	}

	config = Config::getInstance();
    /* Persist first-launch detection once; existing explicit values win. */
    if (ppu_controls_init()) config->save(tempPath);

	idleSecond = 300;

	/* A copied package can retain a config.xml that selects a skin no longer
	 * installed. Recover with the bundled default instead of aborting boot. */
	{
		const char *skin_name =
			config->getStringValue("config/windows/skin/current", "default");
		int skin_length = snprintf(skinPath, sizeof(skinPath), "%sskins/%s/",
		                           applicationPath, skin_name ? skin_name : "default");
		if (skin_length < 0 || skin_length >= (int)sizeof(skinPath)) {
			ppa_boot_fail_screen("Configured skin path is too long");
			return 0;
		}
		if (!Skin::loadSkin(skinPath)) {
			if (skin_name != NULL && strcmp(skin_name, "default") != 0) {
				skin_length = snprintf(skinPath, sizeof(skinPath),
				                       "%sskins/default/", applicationPath);
				if (skin_length < 0 || skin_length >= (int)sizeof(skinPath) ||
				    !Skin::loadSkin(skinPath)) {
					ppa_boot_fail_screen("Could not load configured or default skin");
					return 0;
				}
				/* All subsequent assets and UI clip maps use skinPath. */
			}
			else {
				ppa_boot_fail_screen("Could not load skins/default/skin.xml");
				return 0;
			}
		}
	}

	/* Browser UI font and rasterization preferences.
	 *
	 * The old default opened the 5 MiB WQY TTC through FreeType before the
	 * first frame. On slower Memory Sticks that turns the
	 * "load main_font" phase into a long sequence of tiny random reads.
	 * The main face now boots from the 19 KiB legacy UI font; the i18n layer
	 * independently loads the selected Unicode face and is also used for the
	 * file browser, so multilingual filenames and dialogs retain coverage. */
	if ( (fontManager = FtFontManager::getInstance()) == NULL )
		return 0;
	{
		const char* configuredFace =
			config->getStringValue("config/windows/font/face", "mainfont.ttf");
		const bool fastStartup =
			config->getBooleanValue("config/windows/font/fast_startup", true);
		const char* candidates[6];
		int candidateCount = 0;
		int candidateIndex;

		if (fastStartup)
			candidates[candidateCount++] = "mainfont.ttf";
		if (configuredFace != NULL && configuredFace[0] != 0 &&
		    (!fastStartup || stricmp(configuredFace, "mainfont.ttf") != 0))
			candidates[candidateCount++] = configuredFace;
		candidates[candidateCount++] = "DejaVuSans.ttf";
		candidates[candidateCount++] = "wqy-microhei.ttc";
		candidates[candidateCount] = NULL;

		for (candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
			/* Skip duplicates so a missing configured face is never retried. */
			int duplicate = 0;
			int previousIndex;
			for (previousIndex = 0; previousIndex < candidateIndex; ++previousIndex) {
				if (stricmp(candidates[previousIndex], candidates[candidateIndex]) == 0) {
					duplicate = 1;
					break;
				}
			}
			if (duplicate)
				continue;
			snprintf(tempPath, sizeof(tempPath), "%s%s",
			         fontPath, candidates[candidateIndex]);
			if (fontManager->loadMainFont(tempPath))
				break;
		}
		if (candidateIndex == candidateCount) {
			/* Source-tree/developer layout fallback retained for manual builds. */
			snprintf(tempPath, sizeof(tempPath), "%s%s",
			         applicationPath, "interface/04B_25__.TTF");
			if (!fontManager->loadMainFont(tempPath)) {
				ppa_boot_fail_screen("Could not load any UI font");
				return 0;
			}
		}
	}
	mainFont = fontManager->getMainFont();
	textPixelSize = config->getIntegerValue("config/windows/font/size", 12);
	mainFont->setPixelSize( textPixelSize );
	mainFont->setAntiAlias( config->getBooleanValue("config/windows/font/anti_alias", true) );
	mainFont->setEmbolden( config->getBooleanValue("config/windows/font/embolden", false) );

	fileItemBottom = fileItemTop = fileItemCurrent = fileItemCount = 0;

	initSkinData();

	/* Browser filter and persisted-path state. */
	fileShowHidden = 0;//( config->getBooleanValue("config/filesystem/file_filter/show_hidden", false) ? 1 : 0 );
	fileShowUnknown = 0;//( config->getBooleanValue("config/filesystem/file_filter/show_unknown", false) ? 1 : 0 );

	miniConvSetFileSystemConv( config->getStringValue("config/filesystem/charset/value", "UTF-8") );
	const char* last_path = config->getStringValue("config/filesystem/browser/last_path", "");
	memset(filePath, 0, sizeof(filePath));
	memset(fileShortPath, 0, sizeof(fileShortPath));
	if (last_path != NULL) {
		size_t encodedBytes = strlen(last_path);
		/* A corrupt config must not expand beyond the fixed browser paths.
		 * Decode once, reserve NUL, and reject embedded-NUL path aliases. */
		if (encodedBytes != 0 && encodedBytes % 4U == 0 &&
		    encodedBytes <= ((sizeof(filePath) + 1U) / 3U) * 4U) {
			size_t decodedBytes = (encodedBytes / 4U) * 3U;
			if (last_path[encodedBytes - 1U] == '=') --decodedBytes;
			if (last_path[encodedBytes - 2U] == '=') --decodedBytes;
			if (decodedBytes < sizeof(filePath)) {
				int decoded = base64decode((unsigned char*)filePath,
				                           last_path, (int)encodedBytes);
				if (decoded >= 0 && (size_t)decoded == decodedBytes &&
				    memchr(filePath, 0, decodedBytes) == NULL) {
					filePath[decodedBytes] = 0;
					memcpy(fileShortPath, filePath, decodedBytes + 1U);
				}
				else {
					filePath[0] = 0;
				}
			}
		}
	}

	/* Storage/VFS and controller services. */
	fat_init(sceKernelDevkitVersion());
	ppa_vfs_init();
	if (!ppa_io_background_init())
		;
	if (!ppa_browser_fs_cache_init(fileShowHidden, fileShowUnknown,
	                               movieFileFilter))
		;
#ifdef ENABLE_HPRM
	ctrl_enablehprm(1);
#endif
	// Initialize clock policy and AVC support modules
	cpu_clock_set_cpu_speed(config->getIntegerValue("config/cpu/speed", 66));
	cpu_clock_set_auto_enabled(config->getBooleanValue("config/cpu/auto_clock", true) ? 1 : 0);
	ppa_video_pipeline_reload_config();

//*/
	char* result;
	result = load_codec_prx(applicationPath, sceKernelDevkitVersion());
	if (result!=0){
		return 0;
	}

	/* Required PRXs are resident now. Reserve a high-address arena only when
	 * the active CFW actually exposes a large contiguous Slim/Brite block. */
	{
		int arena_enabled = ppa_memory_pool_init();
		ppa_hardware_profile_refresh_memory();
		(void)arena_enabled;
	}

	gu_font_init();
	memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", fontPath, config->getStringValue("config/subtitles/font/face","subfont.ttf"));
	result=gu_font_load(tempPath);
	if (result==0){
		gu_font_pixelsize_set(config->getIntegerValue("config/subtitles/font/size",16));
		gu_font_scale_set(config->getFloatValue("config/subtitles/font/asc_scale", 2.0),
			config->getFloatValue("config/subtitles/font/multcode_scale", 1.0) );
		float subBorder = config->getFloatValue("config/subtitles/font/border",0.0);
		if ( subBorder > 0.0 ) {
			gu_font_border_enable(1);
			gu_font_border_set(subBorder);
		}
		else
			gu_font_border_enable(0);

		gu_font_color_set(config->getColorValue("config/subtitles/font/color",0xffffff));
		gu_font_border_color_set(config->getColorValue("config/subtitles/font/border_color",0x000000));
		if ( config->getBooleanValue("config/subtitles/font/embolden",true) )
			gu_font_embolden_enable(1);
		else
			gu_font_embolden_enable(0);
		int sub_distance = config->getIntegerValue("config/subtitles/position/distance", 16 );
		/* Bound before negation so a hostile INT_MIN config is not UB. */
		if (sub_distance < -272 || sub_distance > 272) sub_distance = 272;
		else if (sub_distance < 0) sub_distance = -sub_distance;

		if ( stricmp("top", config->getStringValue("config/subtitles/position/align", "bottom") ) == 0 )
			gu_font_align_set(0);
		else
			gu_font_align_set(1);

		gu_font_distance_set(sub_distance);
		miniConvSetDefaultSubtitleConv(config->getStringValue("config/subtitles/charset/value","UTF-8"));

		subtitle_preferences_set_preferred_language(
	config->getStringValue("config/subtitles/preferred_language", "en"));
	}
//*/
	/* TV-output geometry and GU initialization. */
	if ( stricmp("4:3", config->getStringValue("config/tvout/aspect_ratio", "16:9") ) == 0 ) {
		VideoMode::setTVAspectRatio(1);
		setGraphicsTVAspectRatio(1);
	}
	else {
		VideoMode::setTVAspectRatio(0);
		setGraphicsTVAspectRatio(0);
	}
	char overScanValue[16+1];
	snprintf(overScanValue, sizeof(overScanValue), "%s",
	         config->getStringValue("config/tvout/over_scan", "8.0.8.0"));
	int osLeft, osTop, osRight, osBottom;
	osLeft = osTop = osRight = osBottom = 0;
	if (sscanf(overScanValue, "%9d.%9d.%9d.%9d", &osLeft, &osTop, &osRight, &osBottom) != 4) {
		osLeft = osRight = 8;
		osTop = osBottom = 0;
	}
	VideoMode::setTVOverScan(osLeft, osTop, osRight, osBottom);
	setGraphicsTVOverScan(osLeft, osTop, osRight, osBottom);
	setGraphicsTVOutScreen();
	sceGuInit();
	initGraphics(pspType, VideoMode::getVideoMode());
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);

	/* Boot-time presses are useful for capture but must not trigger a menu
	 * command several seconds later. Keep held state, discard queued edges. */
	ctrl_flush();
	return 1;
};

void PpuPlayer::switchFilesystemMode() {
	const char *target;
	if (filesystemMode == 0) {
		if (!ppa_vfs_ntfs_available())
			return;
		target = "ms1:/";
	}
	else {
		target = "ms0:/";
	}
	(void)requestDirectory(target, target, 0);
}

void PpuPlayer::run() {

	ppa_browser_note_activity();
	if (!listDirectory()) {
		memset(filePath, 0, sizeof(filePath));
		memset(fileShortPath, 0, sizeof(fileShortPath));
		(void)listDirectory();
	}
	activeTime = time(NULL);
	ppa_browser_note_activity();
	/* The existing worker can populate the first directory during the fade
	 * and hold. Nothing else submits GU work while this UI owner draws it. */
	beginStartupCredit();
	if (ppa_process_exit_requested())
		return;
	if (!isSuspended)
		paint();
	ppa_browser_note_activity();

	u32 previousKey = 0;
	u64 nextIdlePaintUs = sceKernelGetSystemTimeWide();

	while (!ppa_process_exit_requested()) {

		processPowerEvents();
		if (isSuspended) {
			/* The callback thread only queues power events. Until the matching
			 * resume arrives, keep this thread out of filesystem and GE work. */
			(void)ctrl_wait(5000);
			continue;
		}

		ppa_browser_directory_worker_submit();
		/* Publish only after the same quiet period used for enumeration. */
		if (!ppa_browser_idle_io_expired() &&
		    (startupCredit != NULL || !ctrl_pending()) && commitDirectoryResult()) {
			paint();
			nextIdlePaintUs = sceKernelGetSystemTimeWide() + 250000ULL;
		}

		if (g_ppa_browser_resume_recovery_pending) {
			int list_ok;

			g_ppa_browser_resume_recovery_pending = 0;
			g_ppa_browser_metadata_suppressed = 1;
			ppa_browser_metadata_schedule_wake_cooldown();
			ppa_browser_metadata_worker_cancel(1);
			ppa_browser_directory_worker_cancel(1);
			ppa_browser_fs_cache_cancel_build(1);

			{
				int had_ntfs = ppa_vfs_ntfs_available();
				int attempt;
				int fat_ok = 0;
				int vfs_ok = 0;

				ppa_vfs_shutdown();
				fat_free();

				/* USB/PSPLink wake can report RESUME_COMPLETE before msstor and
				 * the optional NTFS bridge are ready for immediate reopen. Use a
				 * bounded recovery loop on the UI thread after all old workers have
				 * joined; no stale handle survives between attempts. */
				sceKernelDelayThread(350000);
				for (attempt = 0; attempt < 3; ++attempt) {
					fat_ok = fat_init(sceKernelDevkitVersion());
					vfs_ok = ppa_vfs_init();
					if (fat_ok && (!had_ntfs || vfs_ok))
						break;
					ppa_vfs_shutdown();
					fat_free();
					sceKernelDelayThread(200000U +
					                     (unsigned int)attempt * 150000U);
				}
			}
			ppa_browser_fs_cache_resume();

			/* Do not restore a stale cached ms1: listing if the device that was
			 * mounted before suspend did not return after bounded recovery. */
			if (ppa_vfs_is_ntfs_path(filePath) && !ppa_vfs_ntfs_available()) {
				memset(filePath, 0, sizeof(filePath));
				memset(fileShortPath, 0, sizeof(fileShortPath));
			}
			list_ok = listDirectory();
			if (!list_ok) {
				memset(filePath, 0, sizeof(filePath));
				memset(fileShortPath, 0, sizeof(fileShortPath));
				list_ok = listDirectory();
			}
			if (!list_ok)
				g_ppa_browser_metadata_suppressed = 0;

			if (fileItemCount <= 0) {
				fileItemCurrent = 0;
				fileItemTop = 0;
			}
			else {
				if (fileItemCurrent < 0)
					fileItemCurrent = 0;
				if (fileItemCurrent >= fileItemCount)
					fileItemCurrent = fileItemCount - 1;
				if (fileItemTop < 0 || fileItemTop >= fileItemCount)
					fileItemTop = 0;
			}

			filmPreviewReload = filmInformationReload = filmReloadEnable;
			activeTime = time(NULL);
			previousKey = 0;

			paint();
			nextIdlePaintUs = sceKernelGetSystemTimeWide() + 250000ULL;
			continue;
		}

		/* Tribute input never becomes a queued play/delete/config command.
		 * Power recovery above still owns its normal ordered storage handoff. */
		if (serviceStartupCredit())
			continue;

		u32 key = ctrl_pending() ? ctrl_read() : ctrl_wait(5000);

		/* Invalidate unfinished metadata before processing input, without a
		 * worker join or semaphore wait. Held buttons also restart the dwell,
		 * including those without synthetic repeat events. */
		if (key != 0 || ctrl_read_cont() != 0) {
			ppa_browser_metadata_input_activity();
		}
		else if (ppa_browser_idle_io_expired()) {
			__sync_lock_test_and_set(&g_ppa_browser_idle_io_paused, 1U);
		}

        if (key == (PSP_CTRL_CROSS | PSP_CTRL_TRIANGLE)) {
			switchFilesystemMode();
			previousKey = key;
			continue;
		}

		/* A timeout is also represented as zero. Preserve the actual held
		 * state so one-shot chords do not retrigger while held. */
		previousKey = key != 0 ? key : ctrl_read_cont();
		//if ( (key & PSP_CTRL_SELECT) && (key &PSP_CTRL_START) ) {
		//	break;
		//}
		if ( (key & PSP_CTRL_LTRIGGER) && (key & PSP_CTRL_UP) ) {
			fileItemCurrent = 0;
			filmPreviewReload = filmInformationReload = filmReloadEnable;
			activeTime = time(NULL);
		}
		else if ( (key & PSP_CTRL_LTRIGGER) && (key & PSP_CTRL_DOWN) &&
		          fileItemCount > 0 ) {
			fileItemCurrent = fileItemCount - 1;
			filmPreviewReload = filmInformationReload = filmReloadEnable;
			activeTime = time(NULL);
		}
		else if ( (key & PSP_CTRL_LTRIGGER) && (key & PSP_CTRL_SQUARE) ) {
			filmReloadEnable = !filmReloadEnable;
			filmPreviewReload = filmInformationReload = filmReloadEnable;
			activeTime = time(NULL);
		}
		else if ( (key & PSP_CTRL_LTRIGGER) && (key & PSP_CTRL_SELECT) ) {
            /* Retired shortcut; also consume Select so it cannot delete. */
        }
		else if ( (key & PSP_CTRL_LTRIGGER) && (key & PSP_CTRL_TRIANGLE) ) {
			boost::scoped_ptr<VersionDialog> dialog(new (std::nothrow) VersionDialog(Skin::getInstance()->getBackground(skinPath), drawImage));
			if (dialog) {
				if ( dialog->init() ) {
					dialog->execute();
				}
				dialog.reset();
			}
			activeTime = time(NULL);
		}
		else if ( (key & PSP_CTRL_LTRIGGER) && (key & PSP_CTRL_START) ) {
			boost::scoped_ptr<MessageDialog> dialog(new (std::nothrow) MessageDialog(Skin::getInstance()->getBackground(skinPath), drawImage));
			if ( dialog ) {
				if ( dialog->init(i18nGetText(I18N_MSG_DLG_QUIT_TITLE), MESSAGE_TYPE_YES_NO) ) {
					u32 res = dialog->execute();
					if ( res == MESSAGE_RESULT_YES ) {
						/* scoped_ptr also releases the modal image on this exit. */
						break;
					}
				}
				dialog.reset();
			}
			activeTime = time(NULL);
		}
		else if ( key & PSP_CTRL_TRIANGLE ) {
			showPadHelp();
			activeTime = time(NULL);
		}
		else if ( key & PSP_CTRL_SQUARE ) {
			if (browserTextCache != NULL) browserTextCache->clear();
			/* Square is the configuration opener.  Discard the originating
			 * press/repeat before the modal loop so a deliberate long press
			 * cannot be mistaken for an immediate close command. */
			boost::scoped_ptr<ConfigDialog> dialog(new (std::nothrow) ConfigDialog(Skin::getInstance()->getBackground(skinPath), drawImage, uiI18n));
			if ( dialog ) {
				if ( dialog->init() ) {
					dialog->execute();
					char configPath[1024];
					memset(configPath, 0, 1024);
					snprintf(configPath, sizeof(configPath), "%s%s", applicationPath, "config.xml");
					Config::getInstance()->save(configPath);
				}
				/* Destroy borrowed font users before reloading localization. */
				dialog.reset();
				if (uiI18n != NULL)
					uiI18n->load(applicationPath, skinPath);
			}
			activeTime = time(NULL);
		}
		else if ( key & PSP_CTRL_SELECT ) {
			if (fileItems != NULL && fileItemCount > 0 &&
			    fileItemCurrent >= 0 && fileItemCurrent < fileItemCount &&
			    fileItems[fileItemCurrent].filetype != FS_DIRECTORY) {
				boost::scoped_ptr<MessageDialog> dialog(new (std::nothrow) MessageDialog(Skin::getInstance()->getBackground(skinPath), drawImage));
				if ( dialog ) {
					if ( dialog->init(i18nGetText(I18N_MSG_DLG_DELETE_TITLE), MESSAGE_TYPE_YES_NO) ) {
						u32 res = dialog->execute();
						if ( res == MESSAGE_RESULT_YES ) {
							deleteSelectMovie();
							listDirectory();
//							if (fileItemCurrent != 0){
//								fileItemCurrent--;
//							}
//							else {
//								fileItemCurrent = fileItemCount - 1;
//							}
							if (fileItemCurrent >= fileItemCount)
								fileItemCurrent = 0;
							filmPreviewReload = filmInformationReload = filmReloadEnable;
						}
					}
					dialog.reset();
				}
			}
			activeTime = time(NULL);
		}
		else if (key & PSP_CTRL_START) {
			(void)requestDirectory("", "", "");
			activeTime = time(NULL);
		}
		else if( ((key & PSP_CTRL_UP) || (key & CTRL_BACK)) &&
		         fileItemCount > 0 ) {
			if (fileItemCurrent != 0){
				fileItemCurrent--;
			}
			else {
				fileItemCurrent = fileItemCount - 1;
			}
			filmPreviewReload = filmInformationReload = filmReloadEnable;
			activeTime = time(NULL);
		}
		else if( ((key & PSP_CTRL_DOWN) || (key & CTRL_FORWARD)) &&
		         fileItemCount > 0 ){
			if (fileItemCurrent + 1 < fileItemCount) {
				fileItemCurrent++;
			}
			else
				fileItemCurrent = 0;
			filmPreviewReload = filmInformationReload = filmReloadEnable;
			activeTime = time(NULL);
		}
		else if (key & ppu_controls_confirm()) {
			if (fileItems != NULL && fileItemCount > 0 &&
			    fileItemCurrent >= 0 && fileItemCurrent < fileItemCount) {
				if (fileItems[fileItemCurrent].filetype == FS_DIRECTORY) {
					char targetPath[512];
					char targetShortPath[512];
					char restoreName[256];
					const char *compname = fileItems[fileItemCurrent].compname;
					const char *shortname = fileItems[fileItemCurrent].shortname;
					strncpy(targetPath, filePath, sizeof(targetPath) - 1U);
					targetPath[sizeof(targetPath) - 1U] = 0;
					strncpy(targetShortPath, fileShortPath,
					        sizeof(targetShortPath) - 1U);
					targetShortPath[sizeof(targetShortPath) - 1U] = 0;
					restoreName[0] = 0;

					if (compname != NULL && compname[0] != 0) {
						if (strcmp(compname, "..") == 0) {
							ppa_browser_pop_path(targetPath, restoreName,
							                     sizeof(restoreName));
							ppa_browser_pop_path(targetShortPath, NULL, 0);
						}
						else {
							const char *shortComponent =
								ppa_vfs_is_ntfs_path(targetPath) ? compname : shortname;
							size_t pathLength = strlen(targetPath);
							size_t shortLength = strlen(targetShortPath);
							if (pathLength + strlen(compname) + 2 < sizeof(targetPath) &&
							    shortLength + strlen(shortComponent) + 2 <
							        sizeof(targetShortPath)) {
								snprintf(targetPath + pathLength,
								         sizeof(targetPath) - pathLength,
								         "%s/", compname);
								snprintf(targetShortPath + shortLength,
								         sizeof(targetShortPath) - shortLength,
								         "%s/", shortComponent);
							}
							else {
								activeTime = time(NULL);
								continue;
							}
						}
						(void)requestDirectory(targetPath, targetShortPath,
						                       restoreName);
					}
				}
				else {
					/* Playback intent wins over scans/caches. Cancel before even
					 * loading the transitional image. */
					ppa_browser_directory_worker_cancel(1);
					paintLoading();
					playMovie(false);
					filmPreviewReload = filmInformationReload = filmReloadEnable;
				}
			}
			activeTime = time(NULL);
		}
		else if (key & ppu_controls_cancel()) {
			if (fileItems != NULL && fileItemCount > 0 &&
			    fileItemCurrent >= 0 && fileItemCurrent < fileItemCount &&
			    fileItems[fileItemCurrent].filetype != FS_DIRECTORY) {
				ppa_browser_directory_worker_cancel(1);
				paintLoading();
				playMovie(true);
				filmPreviewReload = filmInformationReload = filmReloadEnable;
			}
			activeTime = time(NULL);
		}

		/* A modal action or movie can outlive the input's original window.
		 * Give the returned browser a fresh window before scheduling its work. */
		if (key != 0)
			ppa_browser_note_activity();
		if (key != 0) {
			if (!filmReloadEnable)
				ppa_browser_metadata_clear_cache();
			else if (!g_ppa_browser_metadata_suppressed &&
			         fileItems != NULL && fileItemCount > 0) {
				ppa_browser_metadata_cache_prepare(filePath, fileShortPath,
				                                   fileItems, fileItemCount,
				                                   fileItemCurrent);
				ppa_browser_metadata_selection *selection =
					&g_ppa_browser_metadata_selection;
				if (selection->valid && !selection->scheduled && !selection->result_valid)
					selection->due_us = sceKernelGetSystemTimeWide() +
					                    PPA_BROWSER_METADATA_DWELL_US;
			}
		}

		if (!isSuspended && ( time(NULL) - activeTime >= idleSecond) ) {
			activeTime = time(NULL);
			requestSuspendFromCallback();
			scePowerRequestSuspend();
		}

		{
			u64 nowUs = sceKernelGetSystemTimeWide();
			int inputBacklog = ctrl_pending();

			/* Apply queued intent in a burst instead of doing a full font/GE
			 * repaint after every event. A new physical edge already discards
			 * synthetic repeats, so action buttons cannot sit behind navigation.
			 * The final event paints the resulting state immediately. */
			/* Retain the last composed menu after ten seconds. Repainting a
			 * static filename can still make FreeType read its streamed font;
			 * the card-quiet policy therefore covers the UI as well as scans. */
			if ((key != 0 && !inputBacklog) ||
			    (key == 0 && !inputBacklog && ctrl_read_cont() == 0 &&
			     ppa_browser_input_settled() &&
			     !ppa_browser_idle_io_expired() &&
			     nowUs >= nextIdlePaintUs)) {
				paint();
				nextIdlePaintUs = nowUs + 250000ULL;
			}
			else if (key != 0) {
				nextIdlePaintUs = nowUs;
			}
		}

		if (key == 0 && !ppa_browser_idle_io_expired() &&
		    filmReloadEnable && fileItems != NULL &&
		    fileItemCount > 0) {
			if (ppa_browser_metadata_cache_idle_pump(filePath,
			                                    fileShortPath,
			                                    fileShowHidden,
			                                    fileShowUnknown,
			                                    fileItems,
			                                    fileItemCount,
			                                    fileItemCurrent))
				filmInformationReload = true;
		}
	}
	saveConfig();
};

void PpuPlayer::saveConfig() {
	char last_path[2048];
	base64encode(last_path, (const unsigned char*)filePath, strlen(filePath));

	Config* config = Config::getInstance();
	if ( config ) {
		config->setStringValue("config/filesystem/browser/last_path", last_path);

		char configPath[1024];
		memset(configPath, 0, 1024);
		snprintf(configPath, sizeof(configPath), "%s%s", applicationPath, "config.xml");
		config->save(configPath);
	}
}

void PpuPlayer::releaseStartupCredit() {
	if (startupCredit != NULL) {
		freeImage(startupCredit);
		startupCredit = NULL;
	}
	startupCreditVisibleSinceUs = 0;
	startupCreditFadeStartedUs = 0;
	startupCreditOpacity = 0;
}

void PpuPlayer::beginStartupCredit() {
	char path[sizeof(applicationPath) + 32];
	int length = snprintf(path, sizeof(path), "%scredit.png", applicationPath);
	if (length < 0 || length >= (int)sizeof(path) || ppa_process_exit_requested())
		return;
	startupCredit = loadPNGImage(path);
	/* A bad/missing optional manual-install asset must never break boot.
	 * The supplied 480x272 card is drawn exactly at LCD resolution. */
	if (startupCredit == NULL)
		return;
	if (startupCredit->imageWidth != PSP_SCREEN_WIDTH ||
	    startupCredit->imageHeight != PSP_SCREEN_HEIGHT) {
		releaseStartupCredit();
		return;
	}
	u64 began = sceKernelGetSystemTimeWide();
	for (;;) {
		processPowerEvents();
		if (ppa_process_exit_requested() || isSuspended)
			break;
		u64 elapsed = sceKernelGetSystemTimeWide() - began;
		startupCreditOpacity = elapsed >= PPA_STARTUP_CREDIT_FADE_US ?
			255U : (unsigned int)(elapsed * 255ULL / PPA_STARTUP_CREDIT_FADE_US);
		guStart();
		clearScreen();
		blitImageToScreenWithOpacity(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT,
		                            startupCredit, 0, 0, startupCreditOpacity);
		flipScreen(); /* Includes GE completion and the existing vblank fence. */
		ctrl_flush();
		if (startupCreditOpacity == 255U)
			break;
	}
	startupCreditOpacity = 255U;
	startupCreditVisibleSinceUs = sceKernelGetSystemTimeWide();
}

bool PpuPlayer::serviceStartupCredit() {
	if (startupCredit == NULL)
		return false;
	/* A tribute frame is not controller activity. Resetting the dwell here
	 * would keep a cold first listing parked forever. The bounded readiness
	 * hold below still reveals the menu if slow storage hits the idle cutoff;
	 * revealing it opens a fresh activity window for unfinished work. */
	u64 now = sceKernelGetSystemTimeWide();
	u64 elapsed = now - startupCreditVisibleSinceUs;
	if (startupCreditFadeStartedUs == 0) {
		int menu_ready = fileItemCount > 0 || ppa_browser_directory_worker_idle();
		if (elapsed < PPA_STARTUP_CREDIT_HOLD_US ||
		    (!menu_ready && elapsed < PPA_STARTUP_CREDIT_READY_LIMIT_US)) {
			/* The completed frame stays on scanout throughout the hold. Avoid
			 * redundant menu rasterization or framebuffer copies just to wait. */
			ctrl_flush();
			(void)ctrl_wait(50000);
			return true;
		}
		/* Capture the current menu once; fade frames reuse its CPU UI image. */
		paint();
		/* Menu rasterization can be slow at the idle clock. Start the fade only
		 * after that work, so it cannot consume the entire animation interval. */
		startupCreditFadeStartedUs = sceKernelGetSystemTimeWide();
	}
	elapsed = sceKernelGetSystemTimeWide() - startupCreditFadeStartedUs;
	startupCreditOpacity = elapsed >= PPA_STARTUP_CREDIT_FADE_US ? 0U :
		255U - (unsigned int)(elapsed * 255ULL / PPA_STARTUP_CREDIT_FADE_US);
	presentBrowserFrame();
	ctrl_flush();
	if (startupCreditOpacity == 0U) {
		/* presentBrowserFrame/flipScreen joined the final list, so GE can no
		 * longer read this allocation. No tribute texture survives playback. */
		releaseStartupCredit();
		activeTime = time(NULL);
		ppa_browser_note_activity();
		ppa_browser_metadata_schedule_wake_cooldown();
	}
	return true;
}

void PpuPlayer::paint() {
	clearImage(drawImage, 0);

	paintFileListBox();
	paintDateTime();
	paintBattery();
	paintFilmPreview();
	if (uiI18n != NULL && uiI18n->isReady())
		uiI18n->renderMainLabels(drawImage, filmPreviewUnavailable);
	paintFilmInformation();
	presentBrowserFrame();
};

void PpuPlayer::presentBrowserFrame() {
	Image* mainWindow = Skin::getInstance()->getBackground(skinPath);
	Image* staticOverlay = NULL;
	if (uiI18n != NULL && uiI18n->isReady())
		staticOverlay = uiI18n->getMainStaticOverlay();

	guStart();
	clearScreen();
	blitImageToScreen(0, 0, mainWindow->imageWidth, mainWindow->imageHeight, mainWindow, 0, 0);
	if (staticOverlay != NULL)
		blitAlphaImageToScreen(0, 0, staticOverlay->imageWidth,
		                       staticOverlay->imageHeight, staticOverlay, 0, 0);
	blitAlphaImageToScreen(0, 0, drawImage->imageWidth, drawImage->imageHeight, drawImage, 0, 0);
	if (startupCredit != NULL)
		blitImageToScreenWithOpacity(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT,
		                            startupCredit, 0, 0, startupCreditOpacity);
	flipScreen();
};

bool PpuPlayer::applyDirectoryItems(const char* path,
                                    const char* shortPath,
                                    const char* restoreName,
                                    directory_item_struct* items,
                                    int itemCount,
                                    bool cacheHit)
{
	int i;
	int selected = 0;
	if (items == NULL || itemCount <= 0) {
		if (items != NULL) free(items);
		return false;
	}
	if (fileItems != NULL)
		free(fileItems);
	fileItems = items;
	fileItemCount = itemCount;
	strncpy(filePath, path ? path : "", sizeof(filePath) - 1U);
	filePath[sizeof(filePath) - 1U] = 0;
	strncpy(fileShortPath, shortPath ? shortPath : filePath,
	        sizeof(fileShortPath) - 1U);
	fileShortPath[sizeof(fileShortPath) - 1U] = 0;
	filesystemMode = ppa_vfs_is_ntfs_path(filePath) ? 1 : 0;

	if (restoreName != NULL && restoreName[0] != 0) {
		for (i = 0; i < fileItemCount; ++i) {
			if (fileItems[i].compname != NULL &&
			    strcmp(fileItems[i].compname, restoreName) == 0) {
				selected = i;
				break;
			}
		}
	}
	else if (fileItemCount > 1 && fileItems[1].filetype != FS_DIRECTORY) {
		selected = 1;
	}
	fileItemCurrent = selected;
	fileItemTop = 0;
	filmPreviewReload = filmInformationReload = filmReloadEnable;
	g_ppa_browser_metadata_suppressed = 0;
	ppa_browser_metadata_cache_prepare(filePath, fileShortPath,
	                                   fileItems, fileItemCount,
	                                   fileItemCurrent);
	return true;
}

bool PpuPlayer::requestDirectory(const char* path,
                                 const char* shortPath,
                                 const char* restoreName)
{
	directory_item_struct *cachedItems = NULL;
	int cachedCount = 0;
	int cachedComplete = 0;
	char safePath[512];
	char safeShortPath[512];
	char safeRestoreName[256];
	if (path == NULL) path = "";
	if (shortPath == NULL) shortPath = path;
	if (restoreName == NULL) restoreName = "";
	if (strlen(path) >= sizeof(safePath) ||
	    strlen(shortPath) >= sizeof(safeShortPath) ||
	    strlen(restoreName) >= sizeof(safeRestoreName))
		return false;
	/* listDirectory passes pointers into the current list/path. A cache hit
	 * replaces that list before scheduling its refresh: retain our own input
	 * copies so the restore name never refers to freed directory storage. */
	strcpy(safePath, path);
	strcpy(safeShortPath, shortPath);
	strcpy(safeRestoreName, restoreName);

	/* Input/navigation never joins a parser. Generation cancellation is
	 * immediate; the workers serialize their actual NTFS/FAT access behind a
	 * low-priority gate and leave the UI/input threads runnable. */
	ppa_browser_metadata_clear_cache();
	ppa_browser_note_activity();
	g_ppa_browser_metadata_suppressed = 1;

	if (ppa_browser_fs_cache_get(safePath, safeShortPath,
	                             &cachedItems, &cachedCount,
	                             &cachedComplete)) {
		ppa_browser_directory_worker_cancel(0);
		if (!applyDirectoryItems(safePath, safeShortPath, safeRestoreName,
		                         cachedItems, cachedCount, true)) {
			g_ppa_browser_metadata_suppressed = 0;
			return false;
		}
		/* A PC may have changed this directory since the snapshot was saved.
		 * Refresh it only after navigation settles; showing cached names must
		 * not immediately launch another card scan behind the selector. */
		(void)cachedComplete;
		(void)ppa_browser_directory_worker_schedule(
			safePath, safeShortPath, fileShowHidden, fileShowUnknown,
			safeRestoreName, 1);
		return true;
	}

	if (!ppa_browser_directory_worker_schedule(safePath, safeShortPath,
	                                           fileShowHidden, fileShowUnknown,
	                                           safeRestoreName, 0)) {
		g_ppa_browser_metadata_suppressed = 0;
		return false;
	}
	return true;
}

bool PpuPlayer::listDirectory()
{
	const char *restoreName = "";
	if (fileItems != NULL && fileItemCount > 0 &&
	    fileItemCurrent >= 0 && fileItemCurrent < fileItemCount &&
	    fileItems[fileItemCurrent].compname != NULL)
		restoreName = fileItems[fileItemCurrent].compname;
	return requestDirectory(filePath, fileShortPath, restoreName);
}

bool PpuPlayer::commitDirectoryResult()
{
	ppa_browser_directory_result result;
	if (!ppa_browser_directory_worker_take_result(&result))
		return false;

	if (result.item_count <= 0 || result.items == NULL) {
		if (result.items != NULL) free(result.items);
		g_ppa_browser_metadata_suppressed = 0;
		/* A snapshot can outlive the card or a directory removed on a PC.
		 * Drop only the failed path's stale visible listing; never discard a
		 * different directory the user has navigated to in the meantime. */
		if (result.path[0] != 0 && strcmp(filePath, result.path) == 0 &&
		    fileItemCount > 0) {
			free(fileItems);
			fileItems = NULL;
			fileItemCount = fileItemCurrent = fileItemTop = 0;
		}
		if (fileItemCount <= 0 && result.path[0] != 0)
			(void)requestDirectory("", "", "");
		return true;
	}

	/* The low-priority directory worker already serialized the completed list
	 * into the names-only RAM cache. This UI-side commit only swaps ownership. */
	if (result.refresh && strcmp(filePath, result.path) == 0 &&
	    strcmp(fileShortPath, result.short_path) == 0 &&
	    fileItems != NULL && fileItemCount > 0) {
		int i;
		int oldCurrent = fileItemCurrent;
		int oldTop = fileItemTop;
		int unchanged = fileItemCount == result.item_count;
		char selectedName[256];
		selectedName[0] = 0;
		for (i = 0; unchanged && i < fileItemCount; ++i) {
			unchanged = fileItems[i].filetype == result.items[i].filetype &&
			    strcmp(fileItems[i].longname, result.items[i].longname) == 0 &&
			    strcmp(fileItems[i].shortname, result.items[i].shortname) == 0 &&
			    strcmp(fileItems[i].compname, result.items[i].compname) == 0;
		}
		if (unchanged) {
			/* Do not reset the selector, viewport or completed metadata merely
			 * because the same cached names were confirmed on the card. */
			free(result.items);
			return false;
		}
		if (oldCurrent >= 0 && oldCurrent < fileItemCount) {
			strncpy(selectedName, fileItems[oldCurrent].compname,
			        sizeof(selectedName) - 1U);
			selectedName[sizeof(selectedName) - 1U] = 0;
		}
		/* A refresh belongs to this directory, not its entry-time selection.
		 * Restore the current exact leaf name (NTFS can be case-sensitive). */
		if (!applyDirectoryItems(result.path, result.short_path, selectedName,
		                         result.items, result.item_count, false))
			return false;
		for (i = 0; i < fileItemCount; ++i) {
			if (strcmp(fileItems[i].compname, selectedName) == 0)
				break;
		}
		if (i == fileItemCount) {
			/* The selected file disappeared. Keep the nearest surviving row. */
			fileItemCurrent = oldCurrent < fileItemCount ? oldCurrent : fileItemCount - 1;
			if (fileItemCurrent < 0) fileItemCurrent = 0;
			ppa_browser_metadata_cache_prepare(filePath, fileShortPath,
			                                   fileItems, fileItemCount,
			                                   fileItemCurrent);
		}
		fileItemTop = oldTop >= 0 && oldTop < fileItemCount ? oldTop : 0;
		/* paintFileListBox clamps the preserved viewport around the selection. */
		return true;
	}
	return applyDirectoryItems(result.path, result.short_path,
	                           result.restore_name, result.items,
	                           result.item_count, false);
}

void PpuPlayer::paintBrowserText(FtFont* font, int x, int y, int width, int height,
                                Color color, const char* text)
{
	if (browserTextCache != NULL)
		browserTextCache->draw(font, drawImage, x, y, width, height, color, text);
	else if (font != NULL)
		font->printStringToImage(drawImage, x, y, width, height, color, text);
}

void PpuPlayer::paintFileListBox(){
	FtFont* browserFont = NULL;
	char stringBuffer[512];
	Color textColor;

	/* The UI language face has independent per-glyph Unicode fallback, so
	 * filenames retain their own script regardless of the menu language. */
	if (uiI18n != NULL && uiI18n->isReady())
		browserFont = uiI18n->getFont();
	if (browserFont == NULL && FtFontManager::getInstance() != NULL)
		browserFont = FtFontManager::getInstance()->getMainFont();
	if (browserFont == NULL || fileItems == NULL || fileItemCount <= 0)
		return;
	browserFont->setPixelSize(textPixelSize);

	if ( fileItemCurrent < fileItemTop ) {
		fileItemTop = fileItemCurrent;
	}
	else if (fileItemCurrent - fileItemTop >= fileItemBottom ){
		fileItemTop = fileItemCurrent - fileItemBottom + 1;
	}

	int scrollbarWidth = 0;
	if ( fileItemCount > fileItemBottom )
		scrollbarWidth = 5;
	/* Keep a full viewport's two styles below 384 KiB, leaving space for
	 * clock/battery text. Large custom skins use demand caching instead of
	 * repeatedly evicting and re-rasterizing their own visible rows. */
	const bool primeBothStyles = browserTextCache != NULL &&
		fileListBoxWidth > 0 && fileListBoxWidth <= PSP_SCREEN_WIDTH &&
		textPixelSize > 0 && textPixelSize <= PSP_SCREEN_HEIGHT &&
		fileItemBottom > 0 && fileItemBottom <= 24 &&
		(unsigned int)fileListBoxWidth * (unsigned int)textPixelSize *
		(unsigned int)fileItemBottom * 2U * sizeof(Color) <= 384U * 1024U;

	if ( scrollbarWidth > 0 ) {
		Color lineColor = fileListScrollbarColor;
		int scrollbarPosHeight = (fileListBoxHeight * fileItemBottom) / fileItemCount;
		int scrollbarTravel;
		int x;
		int y;
		if (scrollbarPosHeight < 4)
			scrollbarPosHeight = 4;
		if (scrollbarPosHeight > fileListBoxHeight)
			scrollbarPosHeight = fileListBoxHeight;
		drawLineInImage( drawImage, lineColor, fileListBoxLeft+fileListBoxWidth-4, fileListBoxTop,
			fileListBoxLeft+fileListBoxWidth-4, fileListBoxTop+fileListBoxHeight-1);
		drawLineInImage( drawImage, lineColor, fileListBoxLeft+fileListBoxWidth-1, fileListBoxTop,
			fileListBoxLeft+fileListBoxWidth-1, fileListBoxTop+fileListBoxHeight-1);

		x = fileListBoxLeft+fileListBoxWidth-4;
		scrollbarTravel = fileListBoxHeight - scrollbarPosHeight;
		y = fileItemCount > 1
		  ? (scrollbarTravel * fileItemCurrent) / (fileItemCount - 1)
		  : 0;
		y += fileListBoxTop;
		fillImageRect(drawImage, lineColor, x, y, 4, scrollbarPosHeight);
	}

	int i;
	for(i=0;i<fileItemBottom;i++) {
		if( fileItemTop + i < fileItemCount) {
			const bool selected = (fileItemTop + i == fileItemCurrent);
			const int rowY = fileListBoxTop+i*(2*TEXT_ITEM_BORDER+textPixelSize);
			const int rowHeight = 2*TEXT_ITEM_BORDER+textPixelSize;
			int textX = fileListBoxLeft+TEXT_ITEM_BORDER;
			int textWidth = fileListBoxWidth - 2*TEXT_ITEM_BORDER - scrollbarWidth;
			textColor = fileListTextColor;
			if(selected) {
				Color cursorColor = 0xE0000000 | (fileListCursorColor & 0x00FFFFFF);
				fillImageRect(drawImage, fileListHLBackgroundColor,
					fileListBoxLeft, rowY,
					fileListBoxWidth - scrollbarWidth, rowHeight);
				fillImageRect(drawImage, cursorColor, fileListBoxLeft, rowY,
				                  fileListCursorWidth, rowHeight);
				drawLineInImage(drawImage, cursorColor,
				                fileListBoxLeft, rowY,
				                fileListBoxLeft + fileListBoxWidth - scrollbarWidth - 1, rowY);
				drawLineInImage(drawImage, cursorColor,
				                fileListBoxLeft, rowY + rowHeight - 1,
				                fileListBoxLeft + fileListBoxWidth - scrollbarWidth - 1,
				                rowY + rowHeight - 1);
				textColor = fileListHLTextColor;
				textX += fileListCursorWidth + 2;
				textWidth -= fileListCursorWidth + 2;
			}
			{
				char safe_name[256];
				ppa_utf8_sanitize_copy(safe_name, sizeof(safe_name),
				                       fileItems[fileItemTop + i].longname);
				if (fileItems[fileItemTop + i].filetype == FS_DIRECTORY)
					(void)snprintf(stringBuffer, sizeof(stringBuffer), "<%s>", safe_name);
				else
					(void)snprintf(stringBuffer, sizeof(stringBuffer), "%s", safe_name);
				stringBuffer[sizeof(stringBuffer) - 1U] = 0;
			}
			/* Retain both cursor styles when a row first appears. Subsequent
			 * selection changes need no streamed-font lookup or shaping for
			 * resident rows, including multilingual filenames. */
			if (primeBothStyles) {
				const int normalX = fileListBoxLeft + TEXT_ITEM_BORDER;
				const int normalWidth = fileListBoxWidth - 2*TEXT_ITEM_BORDER - scrollbarWidth;
				browserTextCache->draw(browserFont, drawImage, normalX,
					rowY + textPixelSize-TEXT_ITEM_BORDER,
					normalWidth, textPixelSize, fileListTextColor, stringBuffer, false);
				browserTextCache->draw(browserFont, drawImage,
					normalX + fileListCursorWidth + 2,
					rowY + textPixelSize-TEXT_ITEM_BORDER,
					normalWidth - fileListCursorWidth - 2, textPixelSize,
					fileListHLTextColor, stringBuffer, false);
			}
			paintBrowserText(browserFont, textX,
				rowY + textPixelSize-TEXT_ITEM_BORDER,
				textWidth, textPixelSize, textColor, stringBuffer);
		}
	}
};

void PpuPlayer::paintDateTime() {
	FtFont* mainFont = FtFontManager::getInstance()->getMainFont();
	Color color = Skin::getInstance()->getColorValue("skin/font_color/color", 0xFFFFFF);
	char stringBuffer[64];
	ScePspDateTime currentPSPTime;
	sceRtcGetCurrentClockLocalTime(&currentPSPTime);
	if ( dateVisible ) {
		memset(stringBuffer, 0, 64);
		snprintf(stringBuffer, sizeof(stringBuffer), "%02d/%02d" , currentPSPTime.month, currentPSPTime.day);
		paintBrowserText(mainFont,
			dateLeft, dateTop+textPixelSize-TEXT_ITEM_BORDER, dateWidth, dateHeight,
			color, stringBuffer);
	}
	if ( timeVisible ) {
		memset(stringBuffer, 0, 64);
		snprintf(stringBuffer, sizeof(stringBuffer), "%02d:%02d" , currentPSPTime.hour, currentPSPTime.minute);
		paintBrowserText(mainFont,
			timeLeft, timeTop+textPixelSize-TEXT_ITEM_BORDER, timeWidth, timeHeight,
			color, stringBuffer);
	}
};

void PpuPlayer::paintBattery() {
	FtFont* mainFont = FtFontManager::getInstance()->getMainFont();
	Color color = Skin::getInstance()->getColorValue("skin/font_color/color", 0xFFFFFF);
	char stringBuffer[64];
	int batteryLifePercent = scePowerGetBatteryLifePercent();
	if ( (batteryLifePercent < 0) || (batteryLifePercent > 100) )
		batteryLifePercent = 0;
	//*/
	if ( batteryStatusVisible ) {
		int status = scePowerGetBatteryChargingStatus();
		Image* statusImage;
		if ( (status & PSP_POWER_CB_BATTPOWER) || (status & PSP_POWER_CB_AC_POWER) )
			statusImage = batteryCharging;
		else if ( batteryLifePercent > 66 )
			statusImage = batteryPercent100;
		else if ( batteryLifePercent > 33 )
			statusImage = batteryPercent66;
		else if ( batteryLifePercent > 10 )
			statusImage = batteryPercent33;
		else
			statusImage = batteryPercent10;

		if ( statusImage )
			putImageToImage(statusImage, drawImage, batteryStatusLeft, batteryStatusTop, batteryStatusWidth, batteryStatusHeight);

	}
	//*/
	//*/
	if ( batteryLifeVisible ) {
		memset(stringBuffer, 0, 64);
		snprintf(stringBuffer, sizeof(stringBuffer), "%3d%%" , batteryLifePercent);
		paintBrowserText(mainFont,
			batteryLifeLeft, batteryLifeTop+textPixelSize-TEXT_ITEM_BORDER, batteryLifeWidth, batteryLifeHeight,
			color, stringBuffer);
	}
	//*/
};

void PpuPlayer::paintFilmPreview(){
#if PPA_ENABLE_THUMBNAILS
	char selectedPath[512];
	char selectedKey[640];
	u64 nowUs;
	if ( !filmReloadEnable || fileItems == NULL || fileItemCount <= 0 ||
	     fileItemCurrent < 0 || fileItemCurrent >= fileItemCount )
		return;
	if ( !filmPreviewVisible )
		return;

	if ( filmPreviewReload ) {
		ppa_browser_build_item_path(selectedPath,
		                            sizeof(selectedPath),
		                            filePath,
		                            fileShortPath,
		                            &fileItems[fileItemCurrent]);
		snprintf(selectedKey, sizeof(selectedKey), "%d|%s",
		         fileItems[fileItemCurrent].filetype, selectedPath);
		selectedKey[sizeof(selectedKey) - 1U] = 0;
		nowUs = sceKernelGetSystemTimeWide();

		if (strcmp(g_ppa_browser_preview_key, selectedKey) != 0) {
			strncpy(g_ppa_browser_preview_key, selectedKey,
			        sizeof(g_ppa_browser_preview_key) - 1U);
			g_ppa_browser_preview_key[sizeof(g_ppa_browser_preview_key) - 1U] = 0;
			g_ppa_browser_preview_due_us = nowUs + PPA_BROWSER_PREVIEW_DWELL_US;
			if (filmPreviewImage != NULL) {
				freeImage(filmPreviewImage);
				filmPreviewImage = NULL;
			}
			filmPreviewUnavailable = true;
		}

		/* A pending edge is always serviced before optional preview I/O. */
		if (nowUs < g_ppa_browser_preview_due_us ||
		    !ppa_browser_input_settled())
			return;

		if (!ppa_browser_metadata_io_allowed())
			return;

		if ( filmPreviewImage ) {
			freeImage(filmPreviewImage);
			filmPreviewImage = NULL;
		}
		filmPreviewUnavailable = true;

		if (ppa_browser_is_movie_filetype(fileItems[fileItemCurrent].filetype)) {
			char previewFileName[512];
			memset(previewFileName, 0, 512);

			strncpy(previewFileName, selectedPath,
			        sizeof(previewFileName) - 1U);
			previewFileName[sizeof(previewFileName) - 1U] = 0;
			int filenameEnd = strlen(previewFileName);

			if (filenameEnd > 3) {
				previewFileName[filenameEnd-1] = 'g';
				previewFileName[filenameEnd-2] = 'n';
				previewFileName[filenameEnd-3] = 'p';
				filmPreviewImage = loadPNGImage(previewFileName);
				if (filmPreviewImage != NULL)
					filmPreviewUnavailable = false;
			}
			if ( filmPreviewImage == NULL) {
				char tempPath[1024];
				memset(tempPath, 0, 1024);
				snprintf(tempPath, sizeof(tempPath), "%s%s", skinPath, Skin::getInstance()->getStringValue("skin/film_preview_pannel/preview_image/default_image", "preview.png"));
				filmPreviewImage = loadPNGImage( tempPath );
			}
		}
		g_ppa_browser_preview_due_us = 0;
		filmPreviewReload = false;
	}
	if ( filmPreviewImage )
		putImageToImage(filmPreviewImage, drawImage, filmPreviewLeft, filmPreviewTop, filmPreviewWidth, filmPreviewHeight);
#else
	/* Compile out all preview path construction, PNG I/O/decoding, placeholder
	 * loading, and drawing in the default build. */
	return;
#endif
};

void PpuPlayer::initFilmInformation() {
	filmTotalFrames = 0;
	filmWidth = 0;
	filmHeight = 0;
	filmScale = 1;
	filmRate = 1;
	filmAudioStreams = 0;
	filmSubtitles = 0;
}

void PpuPlayer::paintFilmInformation() {
	/* Selection changes invalidate these values. Do not repeatedly shape
	 * placeholder zeroes (and stream fonts) while the D-pad is active. */
	if (!ppa_browser_input_settled())
		return;
	if ( !filmReloadEnable || fileItems == NULL || fileItemCount <= 0 ||
	     fileItemCurrent < 0 || fileItemCurrent >= fileItemCount )
		return;
	if ( !filmAspectRatioVisible && !filmFpsVisible && !filmTotalTimeVisible && !filmSubtitlesVisible )
		return;
	if ( filmInformationReload ) {
		ppa_browser_film_metadata metadata;
		int used_cache = 0;

		ppa_browser_metadata_set_defaults(&metadata);

		if (ppa_browser_metadata_defer_one_paint_tick()) {
			initFilmInformation();
			used_cache = 1;
		}
		else if (ppa_browser_metadata_cache_get(filePath,
		                                   fileShortPath,
		                                   fileShowHidden,
		                                   fileShowUnknown,
		                                   fileItems,
		                                   fileItemCount,
		                                   fileItemCurrent,
		                                   &metadata)) {
			filmTotalFrames = metadata.total_frames;
			filmWidth = metadata.width;
			filmHeight = metadata.height;
			filmScale = metadata.scale ? metadata.scale : 1;
			filmRate = metadata.rate ? metadata.rate : 1;
			filmAudioStreams = metadata.audio_streams;
			filmSubtitles = metadata.subtitles;
			used_cache = 1;
		}

		if (!used_cache)
			initFilmInformation();
		filmInformationReload = false;
	}
	if (ppa_browser_is_movie_filetype(fileItems[fileItemCurrent].filetype)) {

		FtFont* mainFont = FtFontManager::getInstance()->getMainFont();
		Color color = Skin::getInstance()->getColorValue("skin/font_color/color", 0xFFFFFF);
		char stringBuffer[64];
		if ( filmAspectRatioVisible ) {
			memset(stringBuffer, 0, 64);
			snprintf(stringBuffer, sizeof(stringBuffer), "%d : %d" , filmWidth, filmHeight);
			if (uiI18n != NULL && uiI18n->isReady())
				uiI18n->drawMainValue(drawImage, "aspect_ratio", stringBuffer);
			else
				paintBrowserText(mainFont,
					filmAspectRatioLeft, filmAspectRatioTop+textPixelSize-TEXT_ITEM_BORDER, filmAspectRatioWidth, filmAspectRatioHeight,
					color, stringBuffer);
		}
		if ( filmFpsVisible ) {
			memset(stringBuffer, 0, 64);
			snprintf(stringBuffer, sizeof(stringBuffer), "%6.3f" , filmRate*1.0/filmScale);
			if (uiI18n != NULL && uiI18n->isReady())
				uiI18n->drawMainValue(drawImage, "frames_per_second", stringBuffer);
			else
				paintBrowserText(mainFont,
					filmFpsLeft, filmFpsTop+textPixelSize-TEXT_ITEM_BORDER, filmFpsWidth, filmFpsHeight,
					color, stringBuffer);
		}
		if ( filmTotalTimeVisible ) {
			memset(stringBuffer, 0, 64);
			u64 totalTime = filmTotalFrames;
			totalTime *= filmScale;
			totalTime /= filmRate;
			u32 second = totalTime % 60;
			totalTime /= 60;
			u32 minute = totalTime % 60;
			u32 hour = totalTime / 60;
			snprintf(stringBuffer, sizeof(stringBuffer), "%02d:%02d:%02d" , hour, minute, second);
			if (uiI18n != NULL && uiI18n->isReady())
				uiI18n->drawMainValue(drawImage, "total_time", stringBuffer);
			else
				paintBrowserText(mainFont,
					filmTotalTimeLeft, filmTotalTimeTop+textPixelSize-TEXT_ITEM_BORDER, filmTotalTimeWidth, filmTotalTimeHeight,
					color, stringBuffer);
		}
		if ( filmAudioStreamsVisible ) {
			memset(stringBuffer, 0, 64);
			snprintf(stringBuffer, sizeof(stringBuffer), "%d" , filmAudioStreams);
			if (uiI18n != NULL && uiI18n->isReady())
				uiI18n->drawMainValue(drawImage, "streams", stringBuffer);
			else
				paintBrowserText(mainFont,
					filmAudioStreamsLeft, filmAudioStreamsTop+textPixelSize-TEXT_ITEM_BORDER, filmAudioStreamsWidth, filmAudioStreamsHeight,
					color, stringBuffer);
		}
		if ( filmSubtitlesVisible ) {
			memset(stringBuffer, 0, 64);
			snprintf(stringBuffer, sizeof(stringBuffer), "%d" , filmSubtitles);
			if (uiI18n != NULL && uiI18n->isReady())
				uiI18n->drawMainValue(drawImage, "subtitles", stringBuffer);
			else
				paintBrowserText(mainFont,
					filmSubtitlesLeft, filmSubtitlesTop+textPixelSize-TEXT_ITEM_BORDER, filmSubtitlesWidth, filmSubtitlesHeight,
					color, stringBuffer);
		}
	}

};

void PpuPlayer::paintLoading() {
	char tempPath[1024];
	memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", skinPath, Skin::getInstance()->getStringValue("skin/extra/loading_image", "loading.png") );
	Image* img = loadPNGImage(tempPath);
	if ( img ) {
		guStart();
		clearScreen();
		blitAlphaImageToScreen(0, 0, img->imageWidth, img->imageHeight, img,
			(PSP_SCREEN_WIDTH - img->imageWidth)/2, (PSP_SCREEN_HEIGHT - img->imageHeight)/2);
		flipScreen();
		freeImage(img);
		sceDisplayWaitVblankStart();
	}
};

void PpuPlayer::showPadHelp() {
	char tempPath[1024];
	memset(tempPath, 0, 1024);
	snprintf(tempPath, sizeof(tempPath), "%s%s", skinPath, Skin::getInstance()->getStringValue("skin/extra/pad_help_image", "pad.png") );
	Image* img = loadPNGImage(tempPath);
	if ( img ) {
		Image *helpOverlay = NULL;
		if (uiI18n != NULL && uiI18n->isReady())
			helpOverlay = uiI18n->getHelpStaticOverlay();
		guStart();
		clearScreen();
		blitImageToScreen(0, 0, img->imageWidth, img->imageHeight, img, 0, 0 );
		if (helpOverlay != NULL)
			blitAlphaImageToScreen(0, 0, helpOverlay->imageWidth,
			                       helpOverlay->imageHeight, helpOverlay, 0, 0);
		flipScreen();
		freeImage(img);
	}
	/* Do not let the opening Triangle press immediately satisfy the close
	 * loop after a slow font/PNG draw. */
	ctrl_flush();
	while (ctrl_read_cont() & PSP_CTRL_TRIANGLE)
		sceKernelDelayThread(1000);
	ctrl_flush();
	while (!(ctrl_wait(20000) & PSP_CTRL_TRIANGLE)) {
		/* Event-flag wait leaves the Allegrex runnable only for real input. */
	}
};

void PpuPlayer::deleteSelectMovie() {
	if (filesystemMode == 1 || fileItems == NULL || fileItemCount <= 0 ||
	    fileItemCurrent < 0 || fileItemCurrent >= fileItemCount)
		return;
	ppa_browser_directory_worker_cancel(1);
	ppa_browser_metadata_clear_cache();
	ppa_browser_metadata_worker_cancel(1);
	ppa_browser_fs_cache_cancel_build(1);
	if (!ppa_browser_is_movie_filetype(fileItems[fileItemCurrent].filetype))
		return;
	int deleteFileCount = 1;
	char deleteFiles[5120];

	memset(deleteFiles, 0, sizeof(deleteFiles));
	strncpy(&deleteFiles[0], fileItems[fileItemCurrent].shortname, 511);

	int items = open_directory(filePath, fileShortPath, fileShowHidden,
	                           fileShowUnknown, movieAttachmentFilter,
	                           &attachmentItems);
	int i;
	for (i = 0; i < items && deleteFileCount < 10; ++i) {
		int matched = ppa_browser_attachment_matches(
			fileItems[fileItemCurrent].longname, attachmentItems[i].longname,
			".png", 4);
		if (!matched) {
			struct subtitle_ext_charset_struct *exts = subtitleExt;
			while (exts->ext != NULL) {
				if (ppa_browser_attachment_matches(
					fileItems[fileItemCurrent].longname,
					attachmentItems[i].longname, exts->ext, exts->ext_len)) {
					matched = 1;
					break;
				}
				++exts;
			}
		}
		if (matched)
			strncpy(&deleteFiles[(deleteFileCount++) * 512],
			        attachmentItems[i].shortname, 511);
	}

	char deleteFileName[1024];
	for (i = 0; i < deleteFileCount; ++i) {
		int written = snprintf(deleteFileName, sizeof(deleteFileName), "%s%s",
		                       fileShortPath, &deleteFiles[i * 512]);
		if (written > 0 && written < (int)sizeof(deleteFileName))
			(void)sceIoRemove(deleteFileName);
	}
	ppa_browser_fs_cache_invalidate(filePath, fileShortPath);
};

int PpuPlayer::getSelectMovieSubtitles() {
	if (fileItems == NULL || fileItemCount <= 0 || fileItemCurrent < 0 ||
	    fileItemCurrent >= fileItemCount)
		return 0;
	int items = open_directory(filePath, fileShortPath, fileShowHidden,
	                           fileShowUnknown, movieSubtitleFilter,
	                           &attachmentItems);
	if (items < 1)
		return 0;
	int subtitles = 0;
	for (int i = 0; i < items; ++i) {
		struct subtitle_ext_charset_struct *exts = subtitleExt;
		while (exts->ext != NULL) {
			if (ppa_browser_attachment_matches(
				fileItems[fileItemCurrent].longname,
				attachmentItems[i].longname, exts->ext, exts->ext_len)) {
				++subtitles;
				break;
			}
			++exts;
		}
	}
	return subtitles;
};

void PpuPlayer::fillSelectMovieInfo() {
	if (fileItems == NULL || fileItemCount <= 0 || fileItemCurrent < 0 ||
	    fileItemCurrent >= fileItemCount)
		return;
	memset(currentMovie.movie_file, 0, sizeof(currentMovie.movie_file));

	ppa_browser_build_item_path(currentMovie.movie_file,
	                            sizeof(currentMovie.movie_file),
	                            filePath, fileShortPath,
	                            &fileItems[fileItemCurrent]);

	memset(currentMovie.movie_hash, 0, sizeof(currentMovie.movie_hash));
	sceKernelUtilsMd5Digest((u8 *)currentMovie.movie_file,
	                        strlen(currentMovie.movie_file),
	                        (u8 *)currentMovie.movie_hash);

	currentMovie.movie_subtitle_num = 0;
	int items = open_directory(filePath, fileShortPath, fileShowHidden,
	                           fileShowUnknown, movieSubtitleFilter,
	                           &attachmentItems);
	if (items < 1)
		return;

	for (int i = 0; i < items &&
	                currentMovie.movie_subtitle_num < MAX_MOVIE_SUBTITLES; ++i) {
		struct subtitle_ext_charset_struct *exts = subtitleExt;
		while (exts->ext != NULL) {
			if (ppa_browser_attachment_matches(
				fileItems[fileItemCurrent].longname,
				attachmentItems[i].longname, exts->ext, exts->ext_len)) {
				struct movie_subtitle_struct *subtitle =
					&currentMovie.movie_subtitles[currentMovie.movie_subtitle_num];
				const char *base = strncmp(filePath, "ms1:", 4) == 0 ?
					filePath : fileShortPath;
				const char *name = strncmp(filePath, "ms1:", 4) == 0 ?
					attachmentItems[i].compname : attachmentItems[i].shortname;
				int written;
				memset(subtitle, 0, sizeof(*subtitle));
				written = snprintf(subtitle->subtitle_file,
				                   sizeof(subtitle->subtitle_file), "%s%s",
				                   base ? base : "", name ? name : "");
				if (written <= 0 || written >= (int)sizeof(subtitle->subtitle_file))
					break;
				strncpy(subtitle->subtitle_charset, exts->charset,
				        sizeof(subtitle->subtitle_charset) - 1U);
				++currentMovie.movie_subtitle_num;
				break;
			}
			++exts;
		}
	}
};

void PpuPlayer::playMovie(bool resume) {
	if (browserTextCache != NULL) browserTextCache->clear();

	/* Protect the entire menu-to-playback storage handoff. In particular, do
	 * not let standby interrupt a changed CRC cache write or a worker
	 * join and leave FAT/NTFS ownership half-transitioned. */
	scePowerLock(0);

	/* Playback owns storage/NTFS state. Cancel all nonessential browser work
	 * and reclaim cached menu surfaces before opening subtitles or media. */
	ppa_browser_directory_worker_cancel(1);
	g_ppa_browser_metadata_suppressed = 1;
	ppa_browser_metadata_worker_cancel(1);
	if (!ppa_browser_fs_cache_prepare_playback())
		;
	if (uiI18n != NULL)
		uiI18n->releaseCachedOverlays();
	{
		FtFontManager *manager = FtFontManager::getInstance();
		FtFont *browserFont = manager ? manager->getMainFont() : NULL;
		if (browserFont != NULL)
			browserFont->releaseFallbackFonts();
	}
#if PPA_ENABLE_THUMBNAILS
	if (filmPreviewImage != NULL) {
		freeImage(filmPreviewImage);
		filmPreviewImage = NULL;
	}
	g_ppa_browser_preview_key[0] = 0;
	g_ppa_browser_preview_due_us = 0;
	filmPreviewUnavailable = true;
	filmPreviewReload = filmReloadEnable;
#endif

	fillSelectMovieInfo();

	char* result = NULL;

	int left, top, right, bottom;
	VideoMode::getTVOverScan(left, top, right, bottom);

	int usePos = resume ? 1 : 0;

	result = ppa_play_movie_dispatch(fileItems[fileItemCurrent].filetype, &currentMovie, usePos, pspType, VideoMode::getTVAspectRatio(), left, top, right, bottom, VideoMode::getVideoMode(), uiI18n);

	ppa_cache_wbinv_all();
	if (result) {
		ppa_handle_playback_result(drawImage, currentMovie.movie_file, result);

		(void)ppa_browser_fs_cache_restore_after_playback();
		g_ppa_browser_metadata_suppressed = 0;
		ppa_browser_metadata_schedule_wake_cooldown();
		scePowerUnlock(0);
		return;
	}

	Config* config = Config::getInstance();
	const char* playMode = config->getStringValue("config/player/play_mode", "group");

	if (stricmp("single", playMode) == 0) {
		(void)ppa_browser_fs_cache_restore_after_playback();
		g_ppa_browser_metadata_suppressed = 0;
		ppa_browser_metadata_schedule_wake_cooldown();
		scePowerUnlock(0);
		return;
	}
	else if ( stricmp("group", playMode) == 0  ) {
		while(fileItemCurrent < fileItemCount - 1) {
			if ( is_next_movie( fileItems[fileItemCurrent].longname, fileItems[fileItemCurrent+1].longname ) ) {
				fileItemCurrent++;
				fillSelectMovieInfo();
				int left, top, right, bottom;
				VideoMode::getTVOverScan(left, top, right, bottom);
				result = ppa_play_movie_dispatch(fileItems[fileItemCurrent].filetype, &currentMovie, 0, pspType, VideoMode::getTVAspectRatio(), left, top, right, bottom, VideoMode::getVideoMode(), uiI18n);

				ppa_cache_wbinv_all();
				if (result) {
					ppa_handle_playback_result(drawImage, currentMovie.movie_file, result);
					break;
				}
			}
			else
				break;
		}
	}
	else if ( stricmp("all", playMode) == 0 ) {
		while(fileItemCurrent < fileItemCount - 1) {
			fileItemCurrent++;
			fillSelectMovieInfo();
			int left, top, right, bottom;
			VideoMode::getTVOverScan(left, top, right, bottom);
			result = ppa_play_movie_dispatch(fileItems[fileItemCurrent].filetype, &currentMovie, 0, pspType, VideoMode::getTVAspectRatio(), left, top, right, bottom, VideoMode::getVideoMode(), uiI18n);
			ppa_cache_wbinv_all();
			if (result) {
				ppa_handle_playback_result(drawImage, currentMovie.movie_file, result);
				break;
			}
		}
	}
	(void)ppa_browser_fs_cache_restore_after_playback();

	g_ppa_browser_metadata_suppressed = 0;
	ppa_browser_metadata_schedule_wake_cooldown();
	guStart();
	flipScreen();
	scePowerUnlock(0);
};

enum {
	PPA_POWER_EVENT_SUSPEND = 1U << 0,
	PPA_POWER_EVENT_RESUME  = 1U << 1
};

void PpuPlayer::requestSuspendFromCallback()
{
	__sync_fetch_and_or(&powerEventFlags, PPA_POWER_EVENT_SUSPEND);
}

void PpuPlayer::requestResumeFromCallback()
{
	__sync_fetch_and_or(&powerEventFlags, PPA_POWER_EVENT_RESUME);
}

void PpuPlayer::processPowerEvents()
{
	/* Atomically consume the callback/UI handoff. Separate volatile booleans can
	 * lose an update if the callback preempts between the UI's read and clear.
	 * A PSP may also complete suspend+resume before this thread runs, so process
	 * both bits in their hardware order. */
	unsigned int events = __sync_lock_test_and_set(&powerEventFlags, 0U);
	if (events & PPA_POWER_EVENT_SUSPEND)
		enterSuspendMode();
	if (events & PPA_POWER_EVENT_RESUME)
		leaveSuspendMode();
}

void PpuPlayer::enterSuspendMode()
{
	isSuspended = true;
	g_ppa_browser_metadata_suppressed = 1;

	/* This now runs on the browser thread, never in the power callback. Do not
	 * start new storage writes or wait on storage workers while the Memory Stick
	 * driver is entering standby; playback holds a power lock while movie-state
	 * can change, and resume recovery joins every interrupted menu worker. */
	ppa_browser_directory_worker_cancel(0);
	ppa_browser_metadata_worker_cancel(0);
	ppa_browser_fs_cache_suspend();
	ppa_scratchpad_set_phase(PPA_SCRATCHPAD_PHASE_SUSPENDED);

	gu_font_on_suspend();
	if (uiI18n != NULL)
		uiI18n->onSuspend();
	FtFontManager *manager = FtFontManager::getInstance();
	FtFont *font = manager ? manager->getMainFont() : NULL;
	if (font) font->onSuspend();
}

void PpuPlayer::leaveSuspendMode()
{
	if (browserTextCache != NULL) browserTextCache->clear();
	/* Playback has returned and its codecs are closed before the browser
	 * consumes this event. Firmware may have changed state during standby. */
	ppa_privileged_me_boot_invalidate();
	/* Resume graphics first, then let the normal browser loop rebuild FAT/NTFS
	 * and rejoin any interrupted background task before requesting a listing. */
	FtFontManager *manager = FtFontManager::getInstance();
	FtFont *font = manager ? manager->getMainFont() : NULL;
	if (font) font->onResume();
	if (uiI18n != NULL)
		uiI18n->onResume();
	gu_font_on_resume();
	ppa_scratchpad_set_phase(PPA_SCRATCHPAD_PHASE_MENU);

	ctrl_flush();
	if (startupCredit != NULL) {
		/* Resume always presents a complete tribute before a fresh hold/fade;
		 * no elapsed suspend time can instantly reveal an unrefreshed menu. */
		startupCreditOpacity = 255U;
		startupCreditVisibleSinceUs = sceKernelGetSystemTimeWide();
		startupCreditFadeStartedUs = 0;
	}
	activeTime = time(NULL);
	isSuspended = false;
	g_ppa_browser_metadata_suppressed = 1;
	g_ppa_browser_resume_recovery_pending = 1;
}
