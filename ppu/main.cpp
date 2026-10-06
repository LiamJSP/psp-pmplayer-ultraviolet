#include <new>
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

#include <pspkernel.h>
#include <pspctrl.h>
#include <psppower.h>
#include <pspdebug.h>
#include <psprtc.h>
#include <pspsdk.h>
#include "player.h"
#include "common/ppa_playback_session.h"
#include "common/ppa_process.h"
#include "common/ppa_build_contract.h"
#include "common/ppu_platform_contract.hpp"
#include "common/ppa_hardware_profile.h"
#include "common/ppa_memory.h"
#include "common/ppa_bus_manager.h"
#include "common/ppa_thread_policy.h"
#include "media/VideoPipeline.h"
#include "mod/movie_stat.h"
#include "mod/codec_prx.h"
#include "mod/mp4avcdecoder.h"
#include "mod/audiodecoder.h"

#define VERS 1
#define REVS 0

#include "common/ppa_privileged_bridge.h"

PSP_MODULE_INFO("PMPLAYER_ULTRAVIOLET", 0, VERS, REVS);
PSP_MAIN_THREAD_ATTR(0);
/* Keep the libc heap contiguous on every model. Slim/Brite extended RAM is
 * acquired later as a separate high-address streaming arena after PRX load.
 * The default is the original PSP-1000-safe reservation; builders may tune it
 * explicitly without editing this source, but -1 is intentionally avoided. */
#ifndef PPA_HEAP_SIZE_KB
#define PPA_HEAP_SIZE_KB (18 * 1024)
#endif
PSP_HEAP_SIZE_KB(PPA_HEAP_SIZE_KB);

static PpuPlayer* player;
static volatile unsigned int g_exit_requested;
extern "C" int ppa_process_exit_requested(void)
{
	return __sync_fetch_and_add(&g_exit_requested, 0U) != 0U;
}

/* Power callbakc */
static int power_callback(int arg1, int powerInfo, void * arg)
{
	(void)arg1;
	(void)arg;

	ppa_session_power_callback((unsigned int)powerInfo);
	if (player == 0)
		return 0;

	/* Power callbacks must never touch storage, GE/font state, or wait for
	 * workers. They run on the callback thread and only queue an intent for
	 * the browser UI thread, which performs the ordered transition safely. */
	if ((powerInfo & (PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_STANDBY | PSP_POWER_CB_SUSPENDING)) != 0)
		player->requestSuspendFromCallback();
	if ((powerInfo & PSP_POWER_CB_RESUME_COMPLETE) != 0)
		player->requestResumeFromCallback();

	return 0;
}

/* Exit callback */
static int exit_callback(int arg1, int arg2, void *common){
	(void)arg1;
	(void)arg2;
	(void)common;
	/* The owner thread joins playback, drains ME, then releases code/heap.
	 * Callback context only publishes intent; it never unloads the EBOOT. */
	__sync_lock_test_and_set(&g_exit_requested, 1U);
	return 0;
}

/* Callback thread */
int CallbackThread(SceSize args, void *argp){
	int cbid;

	cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
	sceKernelRegisterExitCallback(cbid);
	cbid = sceKernelCreateCallback("Power Callback", power_callback, NULL);
	scePowerRegisterCallback(0, cbid);

	sceKernelSleepThreadCB();

	return 0;
}

/* Sets up the callback thread and returns its thread id */
int SetupCallbacks(void){
	int thid = 0;

	thid = ppa_thread_create(PPA_THREAD_CALLBACK, "update_thread",
	                         CallbackThread, 0x1000, 0);
	if(thid >= 0)
	{
		sceKernelStartThread(thid, 0, 0);
	}

	return thid;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	pspDebugScreenInit();
	ppa_hardware_profile_init();
	ppa_bus_manager_init();

	SetupCallbacks();

	player = new (std::nothrow) PpuPlayer();

	if (player == 0) {
		pspDebugScreenPrintf("new PpuPlayer failed\n");
		sceKernelDelayThread(3000000);
		sceKernelExitGame();
		return 0;
	}

	if (player->init(0)) {
		player->run();
	}
	else {

		pspDebugScreenPrintf("init fail, press X to exit...\n");

		SceCtrlData input;
		sceCtrlReadBufferPositive(&input, 1);

		while (!(input.Buttons & PSP_CTRL_CROSS) && !ppa_process_exit_requested()) {
			sceKernelDelayThread(10000);
			sceCtrlReadBufferPositive(&input, 1);
		}
	}

	delete player;
	player = NULL;
	ppa_video_pipeline_shutdown();
	/* run/playback only returns after cooperative worker joins. Close any
	 * remaining idle audio workspace, release the retained AVC arena, then
	 * release owned firmware modules before application memory disappears.
	 * The same path covers initialization failures and HOME-requested exit. */
	audio_decoder_close();
	mp4_avc_shutdown();
	(void)unload_codec_prx();
	ppa_privileged_bridge_reset();
	ppa_memory_pool_shutdown();

	sceKernelExitGame();
	return 0;
}
