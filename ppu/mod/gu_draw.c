/*

gu_draw.c
PMPlayer Advance
Copyright (C) 2006 jonny

Homepage: http://jonny.leffe.dnsalias.com
E-mail:   jonny@leffe.dnsalias.com

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
gu routines
*/

#include "gu_draw.h"
#include "media/PlaybackUi.h"
#include "media/VideoPipeline.h"
#include "../common/ppa_cache.h"
#include "../common/ppa_bus_manager.h"
#include "../common/ppa_memory.h"
#include "../common/ppa_video_output.h"
#include "../common/ppa_overlay.h"
#include "../common/ppa_hardware_profile.h"
#include "../common/graphics.h"
#include <psprtc.h>
#include <pspdisplay.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_clock.h"

unsigned int __attribute__((aligned(16))) ppa_gu_list[262144];

static unsigned char __attribute__((aligned(64))) luminosity_textures[64 * number_of_luminosity_boosts] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 4, 4, 4, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 8, 8, 8, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 12, 12, 12, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 16, 16, 16, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 20, 20, 20, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 24, 24, 24, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 28, 28, 28, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 32, 32, 32, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 36, 36, 36, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 40, 40, 40, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 44, 44, 44, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 48, 48, 48, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 52, 52, 52, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 56, 56, 56, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0, 60, 60, 60, 0};

typedef void (*f_ppa_gu_draw)(unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, unsigned int, void *);

//static unsigned char __attribute__((aligned(64))) static_rgb_buffer[0x1E0000];

void *ppa_gu_draw_buffer;
void *ppa_gu_rgb_buffer;

static unsigned int previous_aspect_ratio;
static unsigned int previous_zoom;
static unsigned int previous_subtitle;
static unsigned int previous_info;
static unsigned int previous_interface;
static int previous_psp_type;
static unsigned int output_left;
static unsigned int output_top;
static unsigned int output_width;
static unsigned int output_height;
static unsigned int output_inversion = 0;
static f_ppa_gu_draw g_ppa_gu_draw;
static int g_ppa_gu_video_only = 0;
static int g_ppa_gu_uses_extended_surfaces = 0;
static int g_ppa_gu_direct_scanout = 0;
static SceUID g_ppa_gu_mutex = -1;
static int g_ppa_gu_submission_locked = 0;

static void ppa_gu_tv_draw_target(void)
{
    /* Startup clears and H264X copies may select another destination. Every
     * TV submission restores the eDRAM canvas before drawing/copying fields. */
    sceGuDrawBuffer(GU_PSM_8888, ppa_gu_draw_buffer, 768);
    sceGuOffset(2048 - 360, 2048 - 240);
    sceGuViewport(2048, 2048, 720, 480);
    sceGuScissor(0, 0, 720, 480);
    sceGuEnable(GU_SCISSOR_TEST);
}

/*
 * The decoder and presentation threads can both need the GE: the decoder
 * submits the normal video render while H.264X may need a device-side frame
 * transfer for an immutable recovery/staging surface.  The GU display list is
 * global, so serialize complete submissions (start -> finish -> sync), not
 * merely sceGuStart().  Normal drawing deliberately holds the lock until
 * ppa_gu_wait() because callers submit and wait as two adjacent operations.
 */
static int ppa_gu_mutex_ensure(void)
{
	if (g_ppa_gu_mutex >= 0)
		return 0;

	g_ppa_gu_mutex = sceKernelCreateSema("ppa_gu_mutex", 0, 1, 1, 0);
	return g_ppa_gu_mutex < 0 ? -1 : 0;
}

static int ppa_gu_mutex_lock(void)
{
	SceUInt timeout_us = 1000000U;

	if (ppa_gu_mutex_ensure() < 0)
		return -1;
	/* A playback thread that faults or is forcibly terminated can die while it
	 * owns this semaphore.  Never turn that into a permanent UI/browser hang.
	 * Normal GU submissions complete far inside this one-second ceiling; on a
	 * poisoned semaphore callers fail closed and the recovery display path can
	 * still paint without touching GU state. */
	return sceKernelWaitSema(g_ppa_gu_mutex, 1, &timeout_us) < 0 ? -1 : 0;
}

static void ppa_gu_mutex_unlock(void)
{
	if (g_ppa_gu_mutex >= 0)
		(void)sceKernelSignalSema(g_ppa_gu_mutex, 1);
}

static int ppa_gu_address_is_edram(const void *p)
{
	uintptr_t address;

	if (p == 0)
		return 0;
	address = (uintptr_t)ppa_cached_cptr(p);
	return address >= 0x04000000U && address < 0x04200000U;
}

int ppa_gu_direct_scanout_enabled(void)
{
	return g_ppa_gu_direct_scanout;
}

void *ppa_gu_direct_scanout_surface(unsigned int index)
{
	if (!g_ppa_gu_direct_scanout || index >= PPA_GU_DIRECT_SCANOUT_COUNT)
		return 0;

	return (void *)(uintptr_t)(0x04000000U +
	                          index * PPA_GU_DIRECT_SCANOUT_BYTES);
}

static int ppa_gu_overlay_prepare(void)
{
    return ppa_playback_ui_has_overlay() || ppa_overlay_has_active(sceKernelGetSystemTimeWide());
}

static void ppa_gu_draw_overlay_messages(void)
{
    struct ppa_overlay_message messages[PPA_OVERLAY_MAX_MESSAGES];
    unsigned int count;
    unsigned int i;
    unsigned int old_color;
    unsigned int old_border;
    int old_border_enable;
    int top_left_y = 2;
    int top_right_y = 2;
    int bottom_center_y = 244;
    int center_count = 0;
    int line_step = gu_font_height() + 2;

    ppa_playback_ui_draw();
    /* Health occupies letterbox pixels too. Retire its old digits on the
     * next frame just as for subtitle/message ink outside the video quad. */
    if (ppa_playback_ui_has_overlay())
        previous_info = 1;
    count = ppa_overlay_snapshot(messages, PPA_OVERLAY_MAX_MESSAGES,
                                 sceKernelGetSystemTimeWide());
    if (count == 0)
        return;

    old_color = gu_font_color_get();
    old_border = gu_font_border_color_get();
    old_border_enable = gu_font_border_enable_get();

    for (i = 0; i < count; ++i) {
        int width;
        int x;
        int y;
        int flags = (messages[i].flags & PPA_OVERLAY_UTF8) ? FLAG_UTF8 : 0;
        if (messages[i].flags & PPA_OVERLAY_WHITE) {
            gu_font_color_set(0xffffffffU);
            gu_font_border_color_set(0xff000000U);
            gu_font_border_enable(1);
        }
        width = flags ? gu_font_utf8_width_get(messages[i].text, 0) :
                        gu_font_width_get(messages[i].text, 0);
        switch (messages[i].position) {
        case PPA_OVERLAY_CENTER:
            x = (480 - width) / 2;
            y = (272 - gu_font_height()) / 2 + center_count * line_step;
            center_count++;
            break;
        case PPA_OVERLAY_BOTTOM_CENTER:
            x = (480 - width) / 2;
            y = bottom_center_y;
            bottom_center_y -= line_step;
            break;
        case PPA_OVERLAY_TOP_LEFT:
            x = 4;
            y = top_left_y;
            top_left_y += line_step;
            break;
        case PPA_OVERLAY_TOP_RIGHT:
        default:
            x = 476 - width;
            y = top_right_y;
            top_right_y += line_step;
            break;
        }
        if (x < 2) x = 2;
        if (x > 476) x = 476;
        gu_font_print(x, y, flags, messages[i].text,
                      ppa_gu_scanout_width() == 480U ? output_inversion : 0);
        gu_font_color_set(old_color);
        gu_font_border_color_set(old_border);
        gu_font_border_enable(old_border_enable);
    }
    previous_info = 1;
}

void ppa_gu_draw_without_tvout_supported(unsigned int aspect_ratio, unsigned int zoom, unsigned int luminosity_boost, unsigned int show_interface, unsigned int show_subtitle, unsigned int subtitle_format, unsigned int frame_number, void *video_frame_buffer);
void ppa_gu_draw_psplcd(unsigned int aspect_ratio, unsigned int zoom, unsigned int luminosity_boost, unsigned int show_interface, unsigned int show_subtitle, unsigned int subtitle_format, unsigned int frame_number, void *video_frame_buffer);
void ppa_gu_draw_tvout_interlace(unsigned int aspect_ratio, unsigned int zoom, unsigned int luminosity_boost, unsigned int show_interface, unsigned int show_subtitle, unsigned int subtitle_format, unsigned int frame_number, void *video_frame_buffer);
void ppa_gu_draw_tvout_progressive(unsigned int aspect_ratio, unsigned int zoom, unsigned int luminosity_boost, unsigned int show_interface, unsigned int show_subtitle, unsigned int subtitle_format, unsigned int frame_number, void *video_frame_buffer);

void gu_lcd_output_inversion_set()
	{
	output_inversion = (output_inversion+1) % 2;
	}

void ppa_gu_init_previous_values()
	{
	previous_aspect_ratio = 0xffffffff;
	previous_zoom         = 0xffffffff;
	previous_subtitle     = 1;
	previous_info         = 1;
	previous_interface    = 0;
	}

void ppa_gu_end()
	{
	int direct_scanout;
	int sync_result;
	int handoff_result;

	if (ppa_gu_mutex_lock() < 0)
		return;
	direct_scanout = g_ppa_gu_direct_scanout;

	/* The direct LCD path leaves the display controller scanning one of the
	 * three eDRAM presentation surfaces.  Clearing the current GU draw target
	 * here can therefore modify that surface in the middle of scanout.  First
	 * finish all playback GE work, then let the UI graphics owner copy/switch to
	 * a stable main-RAM framebuffer and restore its 768-pitch GE state. */
	if (direct_scanout) {
		sync_result = sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
		handoff_result = sync_result < 0 ? sync_result :
		                 restoreGraphicsAfterDirectScanout();
		g_ppa_gu_direct_scanout = 0;
		g_ppa_gu_submission_locked = 0;
		ppa_gu_draw_buffer = (void *)0x04000000U;
		ppa_gu_mutex_unlock();
		return;
	}

	sceGuStart(GU_DIRECT, ppa_gu_list);
	sceGuClearColor(0);
	sceGuClear(GU_COLOR_BUFFER_BIT);
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
	ppa_gu_mutex_unlock();
	}

void ppa_gu_start_without_tvout_supported()
	{
	g_ppa_gu_draw = ppa_gu_draw_without_tvout_supported;

	ppa_gu_draw_buffer  = (void *) 0x04000000;
	ppa_gu_rgb_buffer  = ppa_gu_draw_buffer + (4 * 512 * 272);
	/* PSP-1000 keeps both the render target and Sony CSC source in GE eDRAM.
	 * Neither surface needs CPU initialization or a main-RAM cache handoff. */
	}

void ppa_gu_start_psplcd()
	{
	g_ppa_gu_draw = ppa_gu_draw_psplcd;

	ppa_gu_draw_buffer = g_ppa_gu_direct_scanout ?
	                     ppa_gu_direct_scanout_surface(0) :
	                     (void *)0x04000000;
	ppa_gu_rgb_buffer = ppa_memory_extended_rgb_surface();

	/* Evict any cached alias left by a previous decoder session once. From this
	 * point the normal path is Sony CSC -> GE and never asks the CPU to touch the
	 * decoded RGB surface, so no per-frame source cache sweep is required. */
	ppa_cache_wbinv_all();

	/* Direct LCD scanout surfaces are cleared by the GE at the start of every
	 * render. The display stays on the main-RAM bootstrap surface until the
	 * first completed frame, so no CPU initialization pass over eDRAM is needed. */
	}

void ppa_gu_start_tvout_interlace()
	{
	g_ppa_gu_draw = ppa_gu_draw_tvout_interlace;

	ppa_gu_draw_buffer  = (void *) (0x04000000);
	ppa_gu_rgb_buffer  = ppa_memory_extended_rgb_surface();
	ppa_cache_wbinv_all();
	}

void ppa_gu_start_tvout_progressive()
	{
	g_ppa_gu_draw = ppa_gu_draw_tvout_progressive;

	ppa_gu_draw_buffer  = (void *) (0x04000000);
	ppa_gu_rgb_buffer  = ppa_memory_extended_rgb_surface();
	ppa_cache_wbinv_all();
	}

void ppa_gu_start(int psp_type, int tv_aspectratio, int tv_overscan_left, int tv_overscan_top, int tv_overscan_right, int tv_overscan_bottom, int video_mode)
	{
	tv_overscan_left = ppa_video_output_margin(tv_overscan_left, 320);
	tv_overscan_right = ppa_video_output_margin(tv_overscan_right, 320);
	tv_overscan_top = ppa_video_output_margin(tv_overscan_top, 224);
	tv_overscan_bottom = ppa_video_output_margin(tv_overscan_bottom, 224);
	if (ppa_gu_mutex_lock() < 0)
		return;
	sceGuStart(GU_DIRECT, ppa_gu_list);
	sceGuClearColor(0);
	sceGuClear(GU_COLOR_BUFFER_BIT);
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
	ppa_gu_mutex_unlock();
	previous_psp_type = psp_type;
	g_ppa_gu_uses_extended_surfaces =
		m33IsTVOutSupported(psp_type) && ppa_memory_fixed_region_reserved();
	/* 3 * 512 * 272 * 4 = 1,671,168 bytes, safely inside the GE's
	 * 2 MiB eDRAM.  This mode is available only when the decoded RGB source
	 * lives in the reserved 64 MiB main-RAM region, leaving eDRAM entirely for
	 * scanout surfaces. PSP-1000 and TV-out retain their GE-copy paths. */
	/* Output mode must be visible to the pipeline before querying whether
	 * configured effects require a writable intermediate surface. */
	ppa_video_pipeline_set_lcd_output(video_mode == 0);
	g_ppa_gu_direct_scanout =
		g_ppa_gu_uses_extended_surfaces && video_mode == 0 &&
		!ppa_video_pipeline_configured();
	if (!g_ppa_gu_uses_extended_surfaces)
		{
		output_left = 0;
		output_top = 0;
		output_width = 480;
		output_height = 272;
		gu_font_output_set(output_left, output_top, output_width, output_height);
		ppa_gu_start_without_tvout_supported();
		}
	else
		{
			if (video_mode == 0 )
				{
				output_left = 0;
				output_top = 0;
				output_width = 480;
				output_height = 272;
				gu_font_output_set(output_left, output_top, output_width, output_height);
				ppa_gu_start_psplcd();
				}
			else
				{
				output_left = tv_overscan_left;
				output_top = tv_overscan_top;
				output_width = 720 - tv_overscan_left - tv_overscan_right;
				output_height = 480 - tv_overscan_top - tv_overscan_bottom;
				gu_font_output_set(output_left, output_top, output_width, output_height);
				if (video_mode == 1 || video_mode == 2)
					{
					ppa_gu_start_tvout_interlace();
					}
				else
					{
					ppa_gu_start_tvout_progressive();
					}
				}
		}
	}

void ppa_gu_wait()
	{
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
	if (g_ppa_gu_submission_locked) {
		g_ppa_gu_submission_locked = 0;
		ppa_gu_mutex_unlock();
	}
	}

int ppa_gu_clear_frame_blocking(void *destination,
                                unsigned int destination_pitch,
                                unsigned int width,
                                unsigned int height)
{
	unsigned int destination_bytes;
	struct ppa_bus_token ge_bus;

	if (destination == 0 || width == 0U || height == 0U ||
	    width > destination_pitch || destination_pitch > 2048U ||
	    width > 1024U || height > 512U)
		return -1;
	if (ppa_gu_mutex_lock() < 0)
		return -1;

	destination_bytes = destination_pitch * height * 4U;
	if (!ppa_gu_address_is_edram(destination))
		ppa_cache_inv_range(destination, destination_bytes);

	ge_bus = ppa_bus_begin(PPA_BUS_GE);
	sceGuStart(GU_DIRECT, ppa_gu_list);
	sceGuDrawBuffer(GU_PSM_8888, destination, destination_pitch);
	sceGuScissor(0, 0, width, height);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuClearColor(0);
	sceGuClear(GU_COLOR_BUFFER_BIT);
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
	ppa_bus_end(&ge_bus);
	ppa_gu_mutex_unlock();
	return 0;
}

#define PPA_TEXT_PANEL_X 18U
#define PPA_TEXT_PANEL_Y 16U
#define PPA_TEXT_PANEL_W 444U
#define PPA_TEXT_PANEL_H 240U

/* Modal panels retain their bounded 444x240 backup. Center the same logical
 * LCD canvas on TV, and address the two fields just like the video GE copy. */
static unsigned int ppa_gu_panel_x(unsigned int x)
{ return x + (ppa_gu_scanout_width() == 720U ? 120U : 0U); }

static unsigned int ppa_gu_panel_row(unsigned int y)
{
    if (ppa_gu_scanout_width() == 720U) y += 104U;
    return ppa_gu_scanout_interlaced() ?
        y / 2U + ((y & 1U) ? 0U : 262U) : y;
}

unsigned int ppa_gu_text_panel_backup_bytes(void)
{
	return PPA_TEXT_PANEL_W * PPA_TEXT_PANEL_H * 4U;
}

static int ppa_gu_current_framebuffer(void **framebuffer, int *pitch)
{
	int format;
	void *fb;
	int fb_pitch;

	fb = 0;
	fb_pitch = 0;
	format = 0;
	if (sceDisplayGetFrameBuf(&fb, &fb_pitch, &format,
	                          PSP_DISPLAY_SETBUF_IMMEDIATE) < 0)
		return -1;
	if (fb == 0 || fb_pitch <= 0 || fb_pitch > 2048)
		return -1;
	if ((unsigned int)fb_pitch < ppa_gu_panel_x(PPA_TEXT_PANEL_X + PPA_TEXT_PANEL_W))
		return -1;
	if (format != PSP_DISPLAY_PIXEL_FORMAT_8888)
		return -1;
	*framebuffer = fb;
	*pitch = fb_pitch;
	return 0;
}

int ppa_gu_capture_text_panel_region(void *backup, unsigned int bytes)
{
	void *framebuffer;
	int pitch;
	unsigned int y;
	unsigned int *dst;
	const unsigned int *src;

	if (backup == 0 || bytes < ppa_gu_text_panel_backup_bytes())
		return -1;
	if (ppa_gu_current_framebuffer(&framebuffer, &pitch) < 0)
		return -1;
	if (ppa_gu_mutex_lock() < 0)
		return -1;
	dst = (unsigned int *)backup;
	src = (const unsigned int *)ppa_uncached_cptr(framebuffer);
	for (y = 0; y < PPA_TEXT_PANEL_H; ++y) {
		memcpy(dst + y * PPA_TEXT_PANEL_W,
		       src + ppa_gu_panel_row(PPA_TEXT_PANEL_Y + y) * (unsigned int)pitch +
		           ppa_gu_panel_x(PPA_TEXT_PANEL_X),
		       PPA_TEXT_PANEL_W * 4U);
	}
	ppa_gu_mutex_unlock();
	return 0;
}

int ppa_gu_restore_text_panel_region(const void *backup, unsigned int bytes)
{
	void *framebuffer;
	int pitch;
	unsigned int y;
	unsigned int *dst;
	const unsigned int *src;

	if (backup == 0 || bytes < ppa_gu_text_panel_backup_bytes())
		return -1;
	if (ppa_gu_current_framebuffer(&framebuffer, &pitch) < 0)
		return -1;
	if (ppa_gu_mutex_lock() < 0)
		return -1;
	dst = (unsigned int *)ppa_uncached_ptr(framebuffer);
	src = (const unsigned int *)backup;
	for (y = 0; y < PPA_TEXT_PANEL_H; ++y) {
		memcpy(dst + ppa_gu_panel_row(PPA_TEXT_PANEL_Y + y) * (unsigned int)pitch +
		           ppa_gu_panel_x(PPA_TEXT_PANEL_X),
		       src + y * PPA_TEXT_PANEL_W,
		       PPA_TEXT_PANEL_W * 4U);
	}
	ppa_gu_mutex_unlock();
	return 0;
}

/*
 * Paused-playback text panel.
 *
 * Do not submit GU commands here.  Playback deliberately treats GU state as a
 * long-lived context (for example ppa_gu_load() assumes GU_TEXTURE_2D remains
 * enabled).  A modal helper that changes that state can race one final frame
 * while the show thread is entering pause and leave every subsequent frame
 * black.  The menu is static and infrequent, so direct uncached CPU writes to
 * the currently scanned-out 8888 framebuffer are both safer and cheap enough.
 */
static unsigned int ppa_gu_panel_glyph_row(char ch, unsigned int row)
{
	static const unsigned char digits[10][7] = {
		{14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
		{14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
		{2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
		{14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
		{14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
	};
	static const unsigned char letters[26][7] = {
		{14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
		{14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
		{31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
		{14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
		{14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
		{17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
		{17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
		{14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
		{14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
		{15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
		{17,17,17,17,17,17,14}, {17,17,17,17,10,10,4},
		{17,17,17,21,21,27,17}, {17,17,10,4,10,17,17},
		{17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
	};

	if (row >= 7U)
		return 0U;
	if (ch >= '0' && ch <= '9')
		return digits[(unsigned int)(ch - '0')][row];
	if (ch >= 'a' && ch <= 'z')
		ch = (char)(ch - 'a' + 'A');
	if (ch >= 'A' && ch <= 'Z')
		return letters[(unsigned int)(ch - 'A')][row];
	switch (ch) {
	case ' ': return 0U;
	case '.': return row == 6U ? 4U : 0U;
	case ':': return (row == 2U || row == 5U) ? 4U : 0U;
	case ';': return row == 2U ? 4U : row == 5U ? 4U : row == 6U ? 8U : 0U;
	case '/': return (unsigned int)(1U << ((6U - row > 4U) ? 4U : 6U - row));
	case '%': return row == 0U ? 25U : row == 1U ? 26U :
		row == 2U ? 2U : row == 3U ? 4U : row == 4U ? 8U :
		row == 5U ? 11U : 19U;
	case '+': return row == 3U ? 31U :
		(row == 2U || row == 4U) ? 4U : 0U;
	case '-': return row == 3U ? 31U : 0U;
	case '=': return (row == 2U || row == 4U) ? 31U : 0U;
	case '>': return row == 1U ? 16U : row == 2U ? 8U :
		row == 3U ? 4U : row == 4U ? 8U : row == 5U ? 16U : 0U;
	case '<': return row == 1U ? 1U : row == 2U ? 2U :
		row == 3U ? 4U : row == 4U ? 2U : row == 5U ? 1U : 0U;
	case '(' : return row == 1U ? 2U : (row >= 2U && row <= 4U) ? 4U : row == 5U ? 2U : 0U;
	case ')' : return row == 1U ? 8U : (row >= 2U && row <= 4U) ? 4U : row == 5U ? 8U : 0U;
	case '[' : return (row == 0U || row == 6U) ? 14U : 8U;
	case ']' : return (row == 0U || row == 6U) ? 14U : 2U;
	case '?': return row == 0U ? 14U : row == 1U ? 17U :
		row == 2U ? 1U : row == 3U ? 2U : row == 5U ? 4U : 0U;
	default: return 0U;
	}
}

static void ppa_gu_panel_fill(unsigned int *dst, unsigned int pitch,
                              unsigned int x, unsigned int y,
                              unsigned int width, unsigned int height,
                              unsigned int color)
{
	unsigned int yy;
	unsigned int xx;
	for (yy = 0U; yy < height; ++yy) {
		unsigned int *row = dst + ppa_gu_panel_row(y + yy) * pitch + ppa_gu_panel_x(x);
		for (xx = 0U; xx < width; ++xx)
			row[xx] = color;
	}
}

static void ppa_gu_panel_char(unsigned int *dst, unsigned int pitch,
                              unsigned int x, unsigned int y, char ch,
                              unsigned int color)
{
	unsigned int row;
	unsigned int col;
	for (row = 0U; row < 7U; ++row) {
		unsigned int bits = ppa_gu_panel_glyph_row(ch, row);
		for (col = 0U; col < 5U; ++col)
			if ((bits & (1U << (4U - col))) != 0U)
				dst[ppa_gu_panel_row(y + row) * pitch + ppa_gu_panel_x(x + col)] = color;
	}
}

static void ppa_gu_panel_text(unsigned int *dst, unsigned int pitch,
                              unsigned int x, unsigned int y,
                              const char *text, unsigned int color,
                              unsigned int max_x)
{
	while (text != 0 && *text != '\0' && *text != '\n' && x + 5U < max_x) {
		ppa_gu_panel_char(dst, pitch, x, y, *text, color);
		x += 6U;
		++text;
	}
}

static unsigned int ppa_gu_panel_multiline(unsigned int *dst,
                                            unsigned int pitch,
                                            unsigned int x,
                                            unsigned int y,
                                            const char *text,
                                            unsigned int max_lines,
                                            unsigned int color,
                                            unsigned int max_x,
                                            unsigned int max_y)
{
	unsigned int lines = 0U;
	const char *line = text;
	while (line != 0 && *line != '\0' && lines < max_lines && y + 7U < max_y) {
		const char *next;
		ppa_gu_panel_text(dst, pitch, x, y, line, color, max_x);
		next = strchr(line, '\n');
		if (next == 0)
			break;
		line = next + 1;
		y += 15U;
		++lines;
	}
	return y;
}

static int ppa_gu_draw_text_panel_pixels(void *framebuffer,
                                         int pitch,
                                         const char *title,
                                         const char *body,
                                         const char *footer)
{
	unsigned int *dst;
	const unsigned int panel_x = PPA_TEXT_PANEL_X;
	const unsigned int panel_y = PPA_TEXT_PANEL_Y;
	const unsigned int panel_w = PPA_TEXT_PANEL_W;
	const unsigned int panel_h = PPA_TEXT_PANEL_H;
	const unsigned int panel_right = PPA_TEXT_PANEL_X + PPA_TEXT_PANEL_W;
	const unsigned int panel_bottom = PPA_TEXT_PANEL_Y + PPA_TEXT_PANEL_H;

	if (framebuffer == 0 || pitch <= 0 ||
	    (unsigned int)pitch < ppa_gu_panel_x(panel_right))
		return -1;

	dst = (unsigned int *)ppa_uncached_ptr(framebuffer);
	/* Opaque fills are intentional: no read/modify/write of a framebuffer that
	 * the LCD is scanning and no dependence on the GE blend state. */
	ppa_gu_panel_fill(dst, (unsigned int)pitch, panel_x, panel_y,
	                  panel_w, panel_h, 0xff161616U);
	ppa_gu_panel_fill(dst, (unsigned int)pitch, panel_x, panel_y,
	                  panel_w, 18U, 0xff303030U);
	ppa_gu_panel_fill(dst, (unsigned int)pitch, panel_x,
	                  panel_bottom - 20U, panel_w, 20U, 0xff303030U);

	/* One-pixel border. */
	ppa_gu_panel_fill(dst, (unsigned int)pitch, panel_x, panel_y,
	                  panel_w, 1U, 0xffb0b0b0U);
	ppa_gu_panel_fill(dst, (unsigned int)pitch, panel_x, panel_bottom - 1U,
	                  panel_w, 1U, 0xffb0b0b0U);
	ppa_gu_panel_fill(dst, (unsigned int)pitch, panel_x, panel_y,
	                  1U, panel_h, 0xffb0b0b0U);
	ppa_gu_panel_fill(dst, (unsigned int)pitch, panel_right - 1U, panel_y,
	                  1U, panel_h, 0xffb0b0b0U);

	ppa_gu_panel_text(dst, (unsigned int)pitch, panel_x + 8U, panel_y + 6U,
	                  title ? title : "H264X", 0xffffffffU, panel_right - 8U);
	(void)ppa_gu_panel_multiline(dst, (unsigned int)pitch,
	                             panel_x + 8U, panel_y + 28U,
	                             body ? body : "", 11U,
	                             0xffffffffU, panel_right - 8U,
	                             panel_bottom - 24U);
	if (footer != 0 && footer[0] != '\0')
		ppa_gu_panel_text(dst, (unsigned int)pitch,
		                  panel_x + 8U, panel_bottom - 14U,
		                  footer, 0xffd0d0d0U, panel_right - 8U);
	return 0;
}

int ppa_gu_draw_text_panel_blocking(const char *title,
                                    const char *body,
                                    const char *footer)
{
	void *framebuffer;
	int pitch;
	int result;

	if (ppa_gu_current_framebuffer(&framebuffer, &pitch) < 0)
		return -1;
	if (ppa_gu_mutex_lock() < 0)
		return -1;
	result = ppa_gu_draw_text_panel_pixels(framebuffer, pitch,
	                                       title, body, footer);
	ppa_gu_mutex_unlock();
	return result;
}

int ppa_gu_draw_recovery_text_panel_blocking(const char *title,
                                             const char *body,
                                             const char *footer)
{
	int tv_output = ppa_gu_scanout_width() == 720U &&
	                ppa_memory_fixed_region_reserved();
	void *framebuffer = tv_output ? ppa_memory_extended_frame_surface(0) :
	                               (void *)0x04000000U;
	unsigned int *dst = (unsigned int *)ppa_uncached_ptr(framebuffer);
	unsigned int pitch = tv_output ? 768U : 512U;
	unsigned int rows = tv_output ? ppa_gu_scanout_storage_height() : 272U;
	unsigned int i;
	int mode_result;
	int frame_result;

	/* Recovery is called only after container workers have stopped. Do not use
	 * GU state or the GU semaphore here: a failed/forcibly-terminated playback
	 * thread may have left either in an unusable state. A known eDRAM or
	 * reserved TV surface and CPU stores suffice for the error UI. */
	if (!framebuffer) return -1;
	if (tv_output) ppa_cache_inv_range(framebuffer, pitch * rows * 4U);
	for (i = 0; i < pitch * rows; ++i) dst[i] = 0xff000000U;
	__sync_synchronize();
	/* Keep the configured DVE mode on TV. Switching just sceDisplay to LCD
	 * leaves the encoder and VideoMode owner disagreeing after recovery. */
	if (!tv_output) {
		mode_result = sceDisplaySetMode(0, 480, 272);
		if (mode_result < 0) return mode_result;
	}
	(void)sceDisplayWaitVblankStart();
	frame_result = sceDisplaySetFrameBuf(framebuffer, pitch,
	                                     PSP_DISPLAY_PIXEL_FORMAT_8888,
	                                     PSP_DISPLAY_SETBUF_IMMEDIATE);
	if (frame_result < 0)
		return frame_result;

	/* Keep PPA's bookkeeping coherent with the emergency display surface so a
	 * subsequent browser/playback transition does not inherit stale direct-
	 * scanout/TV-out assumptions. No GU command is submitted here. */
	g_ppa_gu_uses_extended_surfaces = tv_output;
	g_ppa_gu_direct_scanout = 0;
	g_ppa_gu_submission_locked = 0;
	if (!tv_output) g_ppa_gu_draw = ppa_gu_draw_without_tvout_supported;
	ppa_gu_draw_buffer = framebuffer;
	ppa_gu_rgb_buffer = tv_output ? ppa_memory_extended_rgb_surface() :
	    (void *)((unsigned char *)framebuffer + (4U * 512U * 272U));
	if (!tv_output) {
		output_left = 0U;
		output_top = 0U;
		output_width = 480U;
		output_height = 272U;
	}
	gu_font_output_set(output_left, output_top, output_width, output_height);
	ppa_video_pipeline_set_lcd_output(!tv_output);

	return ppa_gu_draw_text_panel_pixels(framebuffer, pitch,
	                                     title, body, footer);
}

/* Canvas extent, not coded AVC width: scaling and subtitles can occupy the
 * full visible display. Stride/unused rows need not be copied to a stage. */
unsigned int ppa_gu_scanout_width(void)
{
    return (g_ppa_gu_draw == ppa_gu_draw_tvout_interlace ||
            g_ppa_gu_draw == ppa_gu_draw_tvout_progressive) ? 720U : 480U;
}
unsigned int ppa_gu_scanout_height(void)
{ return ppa_gu_scanout_width() == 720U ? 480U : 272U; }

int ppa_gu_scanout_interlaced(void)
{ return g_ppa_gu_draw == ppa_gu_draw_tvout_interlace; }

unsigned int ppa_gu_scanout_storage_height(void)
{ return ppa_gu_scanout_interlaced() ? 512U : ppa_gu_scanout_height(); }

void ppa_gu_output_rect(unsigned int *left, unsigned int *top,
                        unsigned int *width, unsigned int *height)
{
    if (left) *left = output_left;
    if (top) *top = output_top;
    if (width) *width = output_width;
    if (height) *height = output_height;
}

int ppa_gu_copy_frame_blocking(void *destination,
                               unsigned int destination_pitch,
                               const void *source,
                               unsigned int source_pitch,
                               unsigned int width,
                               unsigned int height)
{
	unsigned int destination_bytes;
	struct ppa_bus_token ge_bus;

	if (destination == 0 || source == 0 || width == 0U || height == 0U ||
	    width > source_pitch || width > destination_pitch ||
	    source_pitch > 2048U || destination_pitch > 2048U ||
	    height > 512U)
		return -1;
	if (ppa_cached_cptr(destination) == ppa_cached_cptr(source))
		return 0;

	destination_bytes = destination_pitch * height * 4U;

	if (ppa_gu_mutex_lock() < 0)
		return -1;

	/* Discard stale CPU aliases before the GE overwrites this destination.
	 * No writeback is needed: old destination bytes are dead, and avoiding it
	 * removes needless DDR traffic. eDRAM has no CPU cache alias. */
	if (!ppa_gu_address_is_edram(destination))
		ppa_cache_inv_range(destination, destination_bytes);

	ge_bus = ppa_bus_begin(PPA_BUS_GE);
	sceGuStart(GU_DIRECT, ppa_gu_list);
	sceGuCopyImage(GU_PSM_8888,
	               0, 0, width, height,
	               source_pitch, source,
	               0, 0, destination_pitch, destination);
	sceGuTexSync();
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
	ppa_bus_end(&ge_bus);
	ppa_gu_mutex_unlock();

	/* The pre-copy invalidate left no cache alias for destination. A later CPU
	 * reader therefore misses and fetches the GE result naturally; display-only
	 * callers incur no second full-frame cache traversal or destination writeback. */
	return 0;
}

static void ppa_gu_load(void *image, int texture_width, int filter)
	{
	sceGuEnable(GU_TEXTURE_2D);
	sceGuDisable(GU_BLEND);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, 512, 512, texture_width, image);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
	sceGuTexFilter(filter, filter);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	}

static void ppa_gu_load_luminosity_texture(void *image)
	{
	sceGuEnable(GU_TEXTURE_2D);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xffffff, 0xffffff);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, 4, 4, 4, image);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	}

static void ppa_gu_load_interface_texture(void *image)
	{
	sceGuEnable(GU_TEXTURE_2D);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, 512, 512, 512, image);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	}

static void ppa_gu_draw_sprite(struct texture_subdivision_struct *t)
	{
	struct vertex_struct *v = sceGuGetMemory(2 * sizeof(struct vertex_struct));

	v[0].texture_x = t->output_texture_x_start;
	v[0].texture_y = t->output_texture_y_start;
	v[0].vertex_x  = (int) t->output_vertex_x_start;
	v[0].vertex_y  = t->output_vertex_y_start;
	v[0].vertex_z  = 0.0;

	v[1].texture_x = t->output_texture_x_end;
	v[1].texture_y = t->output_texture_y_end;
	v[1].vertex_x  = (int) t->output_vertex_x_end;
	v[1].vertex_y  = t->output_vertex_y_end;
	v[1].vertex_z  = 0.0;

	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
	}

static void ppa_gu_draw_sprite_180(struct texture_subdivision_struct *t, int w, int h)
	{
	struct vertex_struct *v = sceGuGetMemory(2 * sizeof(struct vertex_struct));

	v[0].texture_x = t->output_texture_x_start;
	v[0].texture_y = t->output_texture_y_start;
	v[0].vertex_x  = (w) - (int) t->output_vertex_x_start;
	v[0].vertex_y  = (h) - t->output_vertex_y_start;
	v[0].vertex_z  = 0.0;

	v[1].texture_x = t->output_texture_x_end;
	v[1].texture_y = t->output_texture_y_end;
	v[1].vertex_x  = (w) - (int) t->output_vertex_x_end;
	v[1].vertex_y  = (h) - t->output_vertex_y_end;
	v[1].vertex_z  = 0.0;

	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
	}


void ppa_gu_draw(unsigned int aspect_ratio,
                 unsigned int zoom,
                 unsigned int luminosity_boost,
                 unsigned int show_interface,
                 unsigned int show_subtitle,
                 unsigned int subtitle_format,
                 unsigned int frame_number,
                 void *video_frame_buffer) {
	uint64_t t_auto;
	unsigned int elapsed_auto;
	struct ppa_bus_token ge_bus;

	if (ppa_gu_mutex_lock() < 0)
		return;
	g_ppa_gu_submission_locked = 1;
	ge_bus = ppa_bus_begin(PPA_BUS_GE);
	t_auto = cpu_clock_auto_now_us();

	g_ppa_gu_draw(aspect_ratio,
	              zoom,
	              luminosity_boost,
	              show_interface,
	              show_subtitle,
	              subtitle_format,
	              frame_number,
	              video_frame_buffer);
	ppa_bus_end(&ge_bus);

	elapsed_auto = (unsigned int)(cpu_clock_auto_now_us() - t_auto);
	cpu_clock_auto_on_gu_draw_us(elapsed_auto);
}

void ppa_gu_draw_video_only(unsigned int aspect_ratio,
                            unsigned int zoom,
                            unsigned int luminosity_boost,
                            unsigned int subtitle_format,
                            unsigned int frame_number,
                            void *video_frame_buffer)
{
	struct ppa_bus_token ge_bus;
	uint64_t started_us = cpu_clock_auto_now_us();

	if (ppa_gu_mutex_lock() < 0)
		return;
	g_ppa_gu_submission_locked = 1;
	ge_bus = ppa_bus_begin(PPA_BUS_GE);

	/* The pre-filter pass suppresses every overlay, including Health.
	 * Keep video resampling on the GE, but suppress UI/subtitle/info drawing.
	 * The CPU post-process pipeline runs after GU completion and the overlay
	 * helper composites text last, so filters never soften or recolor glyphs. */
	g_ppa_gu_video_only = 1;
	g_ppa_gu_draw(aspect_ratio, zoom, luminosity_boost, 0, 0,
	              subtitle_format, frame_number, video_frame_buffer);
	g_ppa_gu_video_only = 0;
	ppa_bus_end(&ge_bus);

	cpu_clock_auto_on_gu_draw_us(
		(unsigned int)(cpu_clock_auto_now_us() - started_us));
}

void ppa_gu_overlay_cpu_frame(unsigned int show_interface,
                              unsigned int show_subtitle,
                              unsigned int subtitle_format,
                              unsigned int frame_number,
                              void *video_frame_buffer,
                              unsigned int output_texture_width)
{
	unsigned int pitch = output_texture_width;
	unsigned int frame_bytes;
	struct ppa_bus_token ge_bus;

	if (video_frame_buffer == 0 || g_ppa_gu_direct_scanout)
		return;

	if (show_interface == 0 && show_subtitle == 0 && !ppa_gu_overlay_prepare())
		return;

	/* LCD playback uses a 512- or 768-pixel main-RAM pitch.  Reject any
	 * unexpected geometry instead of silently copying with a mismatched stride. */
	if (pitch != 512 && pitch != 768)
		return;

	frame_bytes = output_texture_width * 272U * 4U;
	if (ppa_gu_mutex_lock() < 0)
		return;
	ge_bus = ppa_bus_begin(PPA_BUS_GE);

	/* The CPU enhancement pipeline owns the canonical main-RAM frame.  Publish
	 * it once, then let the GE perform both full-frame transfers around the
	 * overlay draw.  This removes two 480x272 CPU memcpy passes per overlaid
	 * frame while preserving the exact video -> filters -> text ordering. */
	ppa_cache_cpu_wrote_ge_will_read(video_frame_buffer, frame_bytes);

	sceGuStart(GU_DIRECT, ppa_gu_list);
	sceGuDrawBuffer(GU_PSM_8888, ppa_gu_draw_buffer, pitch);
	sceGuOffset(2048 - 240, 2048 - 136);
	sceGuViewport(2048, 2048, 480, 272);
	sceGuScissor(0, 0, 480, 272);
	sceGuEnable(GU_SCISSOR_TEST);

	sceGuCopyImage(GU_PSM_8888,
	               0, 0, 480, 272,
	               output_texture_width, video_frame_buffer,
	               0, 0, pitch, ppa_gu_draw_buffer);
	sceGuTexSync();

	if (show_subtitle) {
		int flags = 0;
		int pwidth = 0;
		struct subtitle_frame_struct *frame = 0;

		if (subtitle_parse_get_frame(&subtitle_parser[show_subtitle - 1],
		                             &frame,
		                             frame_number) == 0 && frame != 0) {
			if ((subtitle_format == 1) &&
			    (gufont_haveflags & GU_FONT_HAS_UNICODE_CHARMAP)) {
				flags = FLAG_UTF8;
				pwidth = gu_font_utf8_width_get(frame->p_string, 0);
			}
			else {
				pwidth = gu_font_width_get(frame->p_string, 0);
			}

			{
				unsigned int sub_distance = gu_font_distance_get();
				if (gu_font_align_get() > 0) {
					gu_font_print((480 - pwidth) / 2,
					              272 - (frame->p_num_lines) * gu_font_height() - sub_distance,
					              flags | FLAG_ALIGN_CENTER,
					              frame->p_string,
					              output_inversion);
				}
				else {
					gu_font_print((480 - pwidth) / 2,
					              sub_distance,
					              flags | FLAG_ALIGN_CENTER,
					              frame->p_string,
					              output_inversion);
				}
			}
			previous_subtitle = 1;
		}
	}

	if (show_interface) {
		struct texture_subdivision_struct texture_subdivision;

		ppa_gu_load_interface_texture(background_8888);
		texture_subdivision_constructor(&texture_subdivision,
		                                480,
		                                interface_height,
		                                16,
		                                output_width,
		                                interface_height * output_height / 272,
		                                output_left,
		                                output_top);
		do {
			texture_subdivision_get(&texture_subdivision);
			ppa_gu_draw_sprite(&texture_subdivision);
		}
		while (texture_subdivision.output_last == 0);
	}

	if (!g_ppa_gu_video_only)
		ppa_gu_draw_overlay_messages();

	/* Rendering and transfer commands are ordered in one list.  The explicit
	 * texture sync separates overlay rasterization from the reverse copy. */
	sceGuTexSync();
	sceGuCopyImage(GU_PSM_8888,
	               0, 0, 480, 272,
	               pitch, ppa_gu_draw_buffer,
	               0, 0, output_texture_width, video_frame_buffer);

	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);

	/* The GE has replaced main-memory pixels that may still have clean cached
	 * aliases from the pre-upload CPU pass.  Invalidate exactly this frame. */
	ppa_cache_device_wrote_cpu_will_read(video_frame_buffer, frame_bytes);
	ppa_bus_end(&ge_bus);
	ppa_gu_mutex_unlock();
}

void ppa_gu_draw_without_tvout_supported(unsigned int aspect_ratio,
                                         unsigned int zoom,
                                         unsigned int luminosity_boost,
                                         unsigned int show_interface,
                                         unsigned int show_subtitle,
                                         unsigned int subtitle_format,
                                         unsigned int frame_number,
                                         void *video_frame_buffer) {
	short texture_width  = aspect_ratios[0].width;
	short texture_height = aspect_ratios[0].height;

	int vertex_width  = aspect_ratios[aspect_ratio].psp_width;
	int vertex_height = aspect_ratios[aspect_ratio].psp_height;

	vertex_width  = zoom * vertex_width  / 100;
	vertex_height = zoom * vertex_height / 100;

	int vertex_x = output_left + (output_width >> 1) - (vertex_width  >> 1);
	int vertex_y = output_top + (output_height >> 1) - (vertex_height >> 1);

	int filter;

	if ((texture_width == vertex_width) && (texture_height == vertex_height))
		filter = GU_NEAREST;
	else
		filter = GU_LINEAR;

	sceGuStart(GU_DIRECT, ppa_gu_list);
	sceGuDrawBuffer(GU_PSM_8888, ppa_gu_draw_buffer, 512);
	sceGuScissor(0, 0, 480, 272);
	sceGuEnable(GU_SCISSOR_TEST);

	if ((previous_aspect_ratio != aspect_ratio) ||
	    (previous_zoom != zoom) ||
	    (previous_interface != show_interface) ||
	    (((unsigned int)vertex_width < output_width || (unsigned int)vertex_height < output_height) &&
	     (previous_subtitle || previous_info))) {

		sceGuClearColor(0);
		sceGuClear(GU_COLOR_BUFFER_BIT);

		previous_aspect_ratio = aspect_ratio;
		previous_zoom         = zoom;
		previous_subtitle     = 0;
		previous_info         = 0;
		previous_interface    = show_interface;
	}

	struct texture_subdivision_struct texture_subdivision;

	if (texture_width > 480) {
		short texture_x = 0;

		if (texture_width == 720 && texture_height == 480) {
			texture_width = 704;
			texture_x = 8;
		}

		int part_width = 480 * vertex_width / texture_width;

		ppa_gu_load(ppa_gu_rgb_buffer + texture_x * 4, 768, filter);

		texture_subdivision_constructor(&texture_subdivision,
		                                480,
		                                texture_height,
		                                16,
		                                part_width,
		                                vertex_height,
		                                vertex_x,
		                                vertex_y);

		do {
			texture_subdivision_get(&texture_subdivision);

			if (output_inversion)
				ppa_gu_draw_sprite_180(&texture_subdivision, 480, 272);
			else
				ppa_gu_draw_sprite(&texture_subdivision);
		}
		while (texture_subdivision.output_last == 0);

		ppa_gu_load(ppa_gu_rgb_buffer + (texture_x + 480) * 4,
		            768,
		            filter);

		texture_subdivision_constructor(&texture_subdivision,
		                                texture_width - 480,
		                                texture_height,
		                                16,
		                                vertex_width - part_width,
		                                vertex_height,
		                                vertex_x + part_width,
		                                vertex_y);

		do {
			texture_subdivision_get(&texture_subdivision);

			if (output_inversion)
				ppa_gu_draw_sprite_180(&texture_subdivision, 480, 272);
			else
				ppa_gu_draw_sprite(&texture_subdivision);
		}
		while (texture_subdivision.output_last == 0);
	}
	else {
		ppa_gu_load(ppa_gu_rgb_buffer, 512, filter);

		texture_subdivision_constructor(&texture_subdivision,
		                                texture_width,
		                                texture_height,
		                                16,
		                                vertex_width,
		                                vertex_height,
		                                vertex_x,
		                                vertex_y);

		do {
			texture_subdivision_get(&texture_subdivision);

			if (output_inversion)
				ppa_gu_draw_sprite_180(&texture_subdivision, 480, 272);
			else
				ppa_gu_draw_sprite(&texture_subdivision);
		}
		while (texture_subdivision.output_last == 0);
	}


	ppa_gu_load_luminosity_texture(luminosity_textures + 64 * luminosity_boost);

	texture_subdivision_constructor(&texture_subdivision,
	                                4,
	                                4,
	                                16,
	                                vertex_width,
	                                vertex_height,
	                                vertex_x,
	                                vertex_y);

	do {
		texture_subdivision_get(&texture_subdivision);
		ppa_gu_draw_sprite(&texture_subdivision);
	}
	while (texture_subdivision.output_last == 0);

	if (show_subtitle) {

		int flags = 0;
		int pwidth = 0;

		struct subtitle_frame_struct *frame = 0;

		if (subtitle_parse_get_frame(&subtitle_parser[show_subtitle - 1],
		                             &frame,
		                             frame_number) == 0) {
			if (frame != 0) {
				if ((subtitle_format == 1) &&
				    (gufont_haveflags & GU_FONT_HAS_UNICODE_CHARMAP)) {
					flags = FLAG_UTF8;
					pwidth = gu_font_utf8_width_get(frame->p_string, 0);
				}
				else {
					pwidth = gu_font_width_get(frame->p_string, 0);
				}

				{
					unsigned int sub_distance = gu_font_distance_get();

					if (gu_font_align_get() > 0) {
						gu_font_print((480 - pwidth) / 2,
						              272 - (frame->p_num_lines) * gu_font_height() - sub_distance,
						              flags | FLAG_ALIGN_CENTER,
						              frame->p_string,
						              output_inversion);
					}
					else {
						gu_font_print((480 - pwidth) / 2,
						              sub_distance,
						              flags | FLAG_ALIGN_CENTER,
						              frame->p_string,
						              output_inversion);
					}
				}

				previous_subtitle = 1;
			}
		}

	}

	if (show_interface) {

		ppa_gu_load_interface_texture(background_8888);

		texture_subdivision_constructor(&texture_subdivision,
		                                480,
		                                interface_height,
		                                16,
		                                output_width,
		                                interface_height * output_height / 272,
		                                output_left,
		                                output_top);

		do {
			texture_subdivision_get(&texture_subdivision);
			ppa_gu_draw_sprite(&texture_subdivision);
		}
		while (texture_subdivision.output_last == 0);

	}

	if (!g_ppa_gu_video_only)
		ppa_gu_draw_overlay_messages();

	sceGuCopyImage(GU_PSM_8888,
	               0,
	               0,
	               480,
	               272,
	               512,
	               ppa_gu_draw_buffer,
	               0,
	               0,
	               512,
	               video_frame_buffer);

	sceGuTexSync();

	sceGuFinish();

}

void ppa_gu_draw_psplcd(unsigned int aspect_ratio,
                        unsigned int zoom,
                        unsigned int luminosity_boost,
                        unsigned int show_interface,
                        unsigned int show_subtitle,
                        unsigned int subtitle_format,
                        unsigned int frame_number,
                        void *video_frame_buffer) {
	short texture_width  = aspect_ratios[0].width;
	short texture_height = aspect_ratios[0].height;

	int vertex_width  = aspect_ratios[aspect_ratio].psp_width;
	int vertex_height = aspect_ratios[aspect_ratio].psp_height;

	vertex_width  = zoom * vertex_width  / 100;
	vertex_height = zoom * vertex_height / 100;

	int vertex_x = output_left + (output_width >> 1) - (vertex_width  >> 1);
	int vertex_y = output_top + (output_height >> 1) - (vertex_height >> 1);

	int filter;

	if ((texture_width == vertex_width) && (texture_height == vertex_height))
		filter = GU_NEAREST;
	else
		filter = GU_LINEAR;

	if (g_ppa_gu_direct_scanout)
		ppa_gu_draw_buffer = video_frame_buffer;

	sceGuStart(GU_DIRECT, ppa_gu_list);
	sceGuDrawBuffer(GU_PSM_8888,
	                ppa_gu_draw_buffer,
	                g_ppa_gu_direct_scanout ?
	                    PPA_GU_DIRECT_SCANOUT_PITCH : 768);
	sceGuScissor(0, 0, 480, 272);
	sceGuEnable(GU_SCISSOR_TEST);

	if (g_ppa_gu_direct_scanout ||
	    (previous_aspect_ratio != aspect_ratio) ||
	    (previous_zoom != zoom) ||
	    (previous_interface != show_interface) ||
	    (((unsigned int)vertex_width < output_width || (unsigned int)vertex_height < output_height) &&
	     (previous_subtitle || previous_info))) {
		sceGuClearColor(0);
		sceGuClear(GU_COLOR_BUFFER_BIT);

		previous_aspect_ratio = aspect_ratio;
		previous_zoom         = zoom;
		previous_subtitle     = 0;
		previous_info         = 0;
		previous_interface    = show_interface;
	}

	struct texture_subdivision_struct texture_subdivision;

	if (texture_width > 480) {
		short texture_x = 0;

		if (texture_width == 720 && texture_height == 480) {
			texture_width = 704;
			texture_x = 8;
		}

		int part_width = 480 * vertex_width / texture_width;

		ppa_gu_load(ppa_gu_rgb_buffer + texture_x * 4, 768, filter);
		texture_subdivision_constructor(&texture_subdivision,
		                                480,
		                                texture_height,
		                                16,
		                                part_width,
		                                vertex_height,
		                                vertex_x,
		                                vertex_y);
		do {
			texture_subdivision_get(&texture_subdivision);
			if (output_inversion)
				ppa_gu_draw_sprite_180(&texture_subdivision, 480, 272);
			else
				ppa_gu_draw_sprite(&texture_subdivision);
		} while (texture_subdivision.output_last == 0);

		ppa_gu_load(ppa_gu_rgb_buffer + (texture_x + 480) * 4, 768, filter);
		texture_subdivision_constructor(&texture_subdivision,
		                                texture_width - 480,
		                                texture_height,
		                                16,
		                                vertex_width - part_width,
		                                vertex_height,
		                                vertex_x + part_width,
		                                vertex_y);
		do {
			texture_subdivision_get(&texture_subdivision);
			if (output_inversion)
				ppa_gu_draw_sprite_180(&texture_subdivision, 480, 272);
			else
				ppa_gu_draw_sprite(&texture_subdivision);
		} while (texture_subdivision.output_last == 0);
	}
	else {
		ppa_gu_load(ppa_gu_rgb_buffer, 512, filter);
		texture_subdivision_constructor(&texture_subdivision,
		                                texture_width,
		                                texture_height,
		                                16,
		                                vertex_width,
		                                vertex_height,
		                                vertex_x,
		                                vertex_y);
		do {
			texture_subdivision_get(&texture_subdivision);
			if (output_inversion)
				ppa_gu_draw_sprite_180(&texture_subdivision, 480, 272);
			else
				ppa_gu_draw_sprite(&texture_subdivision);
		} while (texture_subdivision.output_last == 0);
	}


	ppa_gu_load_luminosity_texture(luminosity_textures + 64 * luminosity_boost);
	texture_subdivision_constructor(&texture_subdivision,
	                                4,
	                                4,
	                                16,
	                                vertex_width,
	                                vertex_height,
	                                vertex_x,
	                                vertex_y);
	do {
		texture_subdivision_get(&texture_subdivision);
		ppa_gu_draw_sprite(&texture_subdivision);
	} while (texture_subdivision.output_last == 0);

	if (show_subtitle) {

		int flags = 0;
		int pwidth = 0;
		struct subtitle_frame_struct *frame = 0;

		if (subtitle_parse_get_frame(&subtitle_parser[show_subtitle - 1],
		                             &frame,
		                             frame_number) == 0) {
			if (frame != 0) {
				if ((subtitle_format == 1) &&
				    (gufont_haveflags & GU_FONT_HAS_UNICODE_CHARMAP)) {
					flags = FLAG_UTF8;
					pwidth = gu_font_utf8_width_get(frame->p_string, 0);
				}
				else {
					pwidth = gu_font_width_get(frame->p_string, 0);
				}

				unsigned int sub_distance = gu_font_distance_get();

				if (gu_font_align_get() > 0) {
					gu_font_print((480 - pwidth) / 2,
					              272 - (frame->p_num_lines) * gu_font_height() - sub_distance,
					              flags | FLAG_ALIGN_CENTER,
					              frame->p_string,
					              output_inversion);
				}
				else {
					gu_font_print((480 - pwidth) / 2,
					              sub_distance,
					              flags | FLAG_ALIGN_CENTER,
					              frame->p_string,
					              output_inversion);
				}

				previous_subtitle = 1;
			}
		}

	}

	if (show_interface) {

		ppa_gu_load_interface_texture(background_8888);
		texture_subdivision_constructor(&texture_subdivision,
		                                480,
		                                interface_height,
		                                16,
		                                output_width,
		                                interface_height * output_height / 272,
		                                output_left,
		                                output_top);
		do {
			texture_subdivision_get(&texture_subdivision);
			ppa_gu_draw_sprite(&texture_subdivision);
		} while (texture_subdivision.output_last == 0);

	}

	if (!g_ppa_gu_video_only)
		ppa_gu_draw_overlay_messages();

	if (!g_ppa_gu_direct_scanout) {
		sceGuCopyImage(GU_PSM_8888,
		               0,
		               0,
		               480,
		               272,
		               768,
		               ppa_gu_draw_buffer,
		               0,
		               0,
		               768,
		               video_frame_buffer);
		sceGuTexSync();
	}

	sceGuFinish();
}
void ppa_gu_draw_tvout_interlace(unsigned int aspect_ratio, unsigned int zoom, unsigned int luminosity_boost, unsigned int show_interface, unsigned int show_subtitle, unsigned int subtitle_format, unsigned int frame_number, void *video_frame_buffer)
	{
	short texture_width  = aspect_ratios[0].width;
	short texture_height = aspect_ratios[0].height;

	int vertex_width  = aspect_ratios[aspect_ratio].psp_width;
	int vertex_height = aspect_ratios[aspect_ratio].psp_height;

	vertex_width  = zoom * vertex_width  / 100;
	vertex_height = zoom * vertex_height / 100;

	int vertex_x = output_left + (output_width >> 1) - (vertex_width  >> 1);
	int vertex_y = output_top + (output_height >> 1) - (vertex_height >> 1);

	int filter;
	if ((texture_width == vertex_width) && (texture_height == vertex_height))
		{
		filter = GU_NEAREST;
		}
	else
		{
		filter = GU_LINEAR;
		}

	sceGuStart(GU_DIRECT, ppa_gu_list);
	ppa_gu_tv_draw_target();
	if ((previous_aspect_ratio != aspect_ratio) || (previous_zoom != zoom) || (previous_interface != show_interface) ||
	    (((unsigned int)vertex_width < output_width || (unsigned int)vertex_height < output_height) && (previous_subtitle || previous_info)))
		{
		sceGuClearColor(0);
		sceGuClear(GU_COLOR_BUFFER_BIT);

		previous_aspect_ratio = aspect_ratio;
		previous_zoom         = zoom;
		previous_subtitle     = 0;
		previous_info         = 0;
		previous_interface = show_interface;
		}

	struct texture_subdivision_struct texture_subdivision;

	if ( texture_width > 480 ) {
		short texture_x = 0;
		if ( texture_width == 720 && texture_height == 480 ) {
			texture_width = 704;
			texture_x = 8;
		}
		int part_width = 480 * vertex_width / texture_width;

		ppa_gu_load(ppa_gu_rgb_buffer+texture_x*4, 768, filter);
		texture_subdivision_constructor(&texture_subdivision, 480, texture_height, 16, part_width, vertex_height, vertex_x, vertex_y);
		do
			{
			texture_subdivision_get(&texture_subdivision);
			ppa_gu_draw_sprite(&texture_subdivision);
			}
		while (texture_subdivision.output_last == 0);

		ppa_gu_load(ppa_gu_rgb_buffer+(texture_x+480)*4, 768, filter);
		texture_subdivision_constructor(&texture_subdivision, texture_width-480, texture_height, 16, vertex_width-part_width, vertex_height, vertex_x+part_width, vertex_y);
		do
			{
			texture_subdivision_get(&texture_subdivision);
			ppa_gu_draw_sprite(&texture_subdivision);
			}
		while (texture_subdivision.output_last == 0);

	}
	else {

		ppa_gu_load(ppa_gu_rgb_buffer, 512, filter);
		texture_subdivision_constructor(&texture_subdivision, texture_width, texture_height, 16, vertex_width, vertex_height, vertex_x, vertex_y);
		do
			{
			texture_subdivision_get(&texture_subdivision);
			ppa_gu_draw_sprite(&texture_subdivision);
			}
		while (texture_subdivision.output_last == 0);

	}

	ppa_gu_load_luminosity_texture(luminosity_textures + 64 * luminosity_boost);
	texture_subdivision_constructor(&texture_subdivision, 4, 4, 16, vertex_width, vertex_height, vertex_x, vertex_y);
	do
		{
		texture_subdivision_get(&texture_subdivision);
		ppa_gu_draw_sprite(&texture_subdivision);
		}
	while (texture_subdivision.output_last == 0);

	if (show_subtitle )
		{
		int flags = 0;
		int pwidth = 0;

		struct subtitle_frame_struct *frame = 0;
		if (subtitle_parse_get_frame(&subtitle_parser[show_subtitle-1], &frame, frame_number)==0)
			{
				if (frame!=0)
					{
					if ((subtitle_format==1) && (gufont_haveflags&GU_FONT_HAS_UNICODE_CHARMAP))
						{
						flags = FLAG_UTF8;
						pwidth = gu_font_utf8_width_get(frame->p_string,0);
						}
					else
						pwidth = gu_font_width_get(frame->p_string,0);
					unsigned int sub_distance = gu_font_distance_get();
					if ( gu_font_align_get() > 0 )
						gu_font_print( (480-pwidth)/2, 272-(frame->p_num_lines)*gu_font_height()-sub_distance, flags | FLAG_ALIGN_CENTER, frame->p_string, 0);
					else
						gu_font_print( (480-pwidth)/2, sub_distance, flags | FLAG_ALIGN_CENTER, frame->p_string, 0);
					//gu_font_print( (480-pwidth)/2, gu_font_height(), flags | FLAG_ALIGN_CENTER, frame->p_string);

					previous_subtitle = 1;//(pwidth<=vertex_width?(vertex_height<272?1:0):1);
					}
			}
		}

	if (show_interface)
		{
		ppa_gu_load_interface_texture(background_8888);
		texture_subdivision_constructor(&texture_subdivision, 480, interface_height, 16, output_width, interface_height*output_height/272, output_left, output_top);
		do
			{
			texture_subdivision_get(&texture_subdivision);
			ppa_gu_draw_sprite(&texture_subdivision);
			}
		while (texture_subdivision.output_last == 0);
		}

	if (!g_ppa_gu_video_only)
		ppa_gu_draw_overlay_messages();

	int i;
	void* s0 = ppa_gu_draw_buffer;
	void* d0 = video_frame_buffer;
	void* d1 = d0 + 804864;
	for(i=0; i<240; i++)
		{
		sceGuCopyImage(GU_PSM_8888, 0, 0, 720, 1, 768, s0, 0, 0, 768, d1);
		sceGuTexSync();
		s0+=3072;
		d1+=3072;
		sceGuCopyImage(GU_PSM_8888, 0, 0, 720, 1, 768, s0, 0, 0, 768, d0);
		sceGuTexSync();
		s0+=3072;
		d0+=3072;
		}
	sceGuFinish();
	}

void ppa_gu_draw_tvout_progressive(unsigned int aspect_ratio, unsigned int zoom, unsigned int luminosity_boost, unsigned int show_interface, unsigned int show_subtitle, unsigned int subtitle_format, unsigned int frame_number, void *video_frame_buffer)
	{
	short texture_width  = aspect_ratios[0].width;
	short texture_height = aspect_ratios[0].height;

	int vertex_width  = aspect_ratios[aspect_ratio].psp_width;
	int vertex_height = aspect_ratios[aspect_ratio].psp_height;

	vertex_width  = zoom * vertex_width  / 100;
	vertex_height = zoom * vertex_height / 100;

	int vertex_x = output_left + (output_width >> 1) - (vertex_width  >> 1);
	int vertex_y = output_top + (output_height >> 1) - (vertex_height >> 1);

	int filter;
	if ((texture_width == vertex_width) && (texture_height == vertex_height))
		{
		filter = GU_NEAREST;
		}
	else
		{
		filter = GU_LINEAR;
		}

	sceGuStart(GU_DIRECT, ppa_gu_list);
	ppa_gu_tv_draw_target();
	if ((previous_aspect_ratio != aspect_ratio) || (previous_zoom != zoom) || (previous_interface != show_interface) ||
	    (((unsigned int)vertex_width < output_width || (unsigned int)vertex_height < output_height) && (previous_subtitle || previous_info)))
		{
		sceGuClearColor(0);
		sceGuClear(GU_COLOR_BUFFER_BIT);

		previous_aspect_ratio = aspect_ratio;
		previous_zoom         = zoom;
		previous_subtitle     = 0;
		previous_info         = 0;
		previous_interface = show_interface;
		}

	struct texture_subdivision_struct texture_subdivision;

	if ( texture_width > 512 ) {
		short texture_x = 0;
		if ( texture_width == 720 && texture_height == 480 ) {
			texture_width = 704;
			texture_x = 8;
		}
		int part_width = 512 * vertex_width / texture_width;

		ppa_gu_load(ppa_gu_rgb_buffer+texture_x*4, 768, filter);
		texture_subdivision_constructor(&texture_subdivision, 512, texture_height, 16, part_width, vertex_height, vertex_x, vertex_y);
		do
			{
			texture_subdivision_get(&texture_subdivision);
			ppa_gu_draw_sprite(&texture_subdivision);
			}
		while (texture_subdivision.output_last == 0);

		ppa_gu_load(ppa_gu_rgb_buffer+(texture_x+512)*4, 768, filter);
		texture_subdivision_constructor(&texture_subdivision, texture_width-512, texture_height, 16, vertex_width-part_width, vertex_height, vertex_x+part_width, vertex_y);
		do
			{
			texture_subdivision_get(&texture_subdivision);
			ppa_gu_draw_sprite(&texture_subdivision);
			}
		while (texture_subdivision.output_last == 0);

	}
	else {
		/* CSC pitch changes above 480 even when one 512-wide texture fits. */
		ppa_gu_load(ppa_gu_rgb_buffer, texture_width > 480 ? 768 : 512, filter);
		texture_subdivision_constructor(&texture_subdivision, texture_width, texture_height, 16, vertex_width, vertex_height, vertex_x, vertex_y);
		do
			{
			texture_subdivision_get(&texture_subdivision);
			ppa_gu_draw_sprite(&texture_subdivision);
			}
		while (texture_subdivision.output_last == 0);
	}

	ppa_gu_load_luminosity_texture(luminosity_textures + 64 * luminosity_boost);
	texture_subdivision_constructor(&texture_subdivision, 4, 4, 16, vertex_width, vertex_height, vertex_x, vertex_y);
	do
		{
		texture_subdivision_get(&texture_subdivision);
		ppa_gu_draw_sprite(&texture_subdivision);
		}
	while (texture_subdivision.output_last == 0);

	if (show_subtitle )
		{
		int flags = 0;
		int pwidth = 0;

		struct subtitle_frame_struct *frame = 0;
		if (subtitle_parse_get_frame(&subtitle_parser[show_subtitle-1], &frame, frame_number)==0)
			{
				if (frame!=0)
					{
					if ((subtitle_format==1) && (gufont_haveflags&GU_FONT_HAS_UNICODE_CHARMAP))
						{
						flags = FLAG_UTF8;
						pwidth = gu_font_utf8_width_get(frame->p_string,0);
						}
					else
						pwidth = gu_font_width_get(frame->p_string,0);
					unsigned int sub_distance = gu_font_distance_get();
					if ( gu_font_align_get() > 0 )
						gu_font_print( (480-pwidth)/2, 272-(frame->p_num_lines)*gu_font_height()-sub_distance, flags | FLAG_ALIGN_CENTER, frame->p_string, 0);
					else
						gu_font_print( (480-pwidth)/2, sub_distance, flags | FLAG_ALIGN_CENTER, frame->p_string, 0);
					//gu_font_print( (480-pwidth)/2, gu_font_height(), flags | FLAG_ALIGN_CENTER, frame->p_string);

					previous_subtitle = 1;//(pwidth<=vertex_width?(vertex_height<272?1:0):1);
					}
			}
		}

	if (show_interface)
		{
		ppa_gu_load_interface_texture(background_8888);
		texture_subdivision_constructor(&texture_subdivision, 480, interface_height, 16, output_width, interface_height*output_height/272, output_left, output_top);
		do
			{
			texture_subdivision_get(&texture_subdivision);
			ppa_gu_draw_sprite(&texture_subdivision);
			}
		while (texture_subdivision.output_last == 0);
		}

	if (!g_ppa_gu_video_only)
		ppa_gu_draw_overlay_messages();

	sceGuCopyImage(GU_PSM_8888, 0, 0, 720, 480, 768, ppa_gu_draw_buffer, 0, 0, 768, video_frame_buffer);
	sceGuTexSync();

	sceGuFinish();
	}

typedef struct tagBITMAPFILEHEADER {
	uint16_t bfType;
	uint32_t bfSize;
	uint16_t bfReserved1;
	uint16_t bfReserved2;
	uint32_t bfOffBits;
} __attribute__((packed)) BITMAPFILEHEADER ;

typedef struct tagBITMAPINFOHEADER{
	uint32_t biSize;
	int32_t biWidth;
	int32_t biHeight;
	uint16_t biPlanes;
	uint16_t biBitCount;
	uint32_t biCompression;
	uint32_t biSizeImage;
	int32_t biXPelsPerMeter;
	int32_t biYPelsPerMeter;
	int32_t biClrUsed;
	int32_t biClrImportant;
} BITMAPINFOHEADER;

void make_bmp_screenshot() {
	int x, y, mode, pixel_format, width, height, texture_width;
	uint32_t* frame_buffer;
	unsigned char buffer[1440];
	char filename[512];
	SceUID fd;

	sceDisplayGetMode(&mode, &width, &height);
	if(width > 480)
		return;
	sceDisplayGetFrameBuf((void **)&frame_buffer, &texture_width, &pixel_format, 0);

	BITMAPFILEHEADER h1;
	BITMAPINFOHEADER h2;

	sceIoMkdir("ms0:/PICTURE", 0777);
	sceIoMkdir("ms0:/PICTURE/PPU", 0777);

	memset(filename, 0, 512);

	ScePspDateTime current_time;
	sceRtcGetCurrentClockLocalTime(&current_time);

	snprintf(filename, sizeof(filename), "ms0:/PICTURE/PPU/SNAPSHOT%04d%02d%02d%05d.BMP",
		current_time.year,
		current_time.month,
		current_time.day,
		current_time.hour*3600+current_time.minute*60+current_time.second);
	fd = sceIoOpen(filename, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if(fd < 0)
		return;

	h1.bfType = 0x4D42;
	h1.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + 3*width*height;
	h1.bfReserved1 = 0;
	h1.bfReserved2 = 0;
	h1.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

	h2.biSize = sizeof(BITMAPINFOHEADER);
	h2.biWidth = width;
	h2.biHeight = height;
	h2.biPlanes = 1;
	h2.biBitCount = 24;
	h2.biCompression = 0;
	h2.biSizeImage = 3*width*height;
	h2.biXPelsPerMeter = 0;
	h2.biYPelsPerMeter = 0;
	h2.biClrUsed = 0;
	h2.biClrImportant = 0;

	sceIoWrite(fd, &h1, sizeof(BITMAPFILEHEADER));
	sceIoWrite(fd, &h2, sizeof(BITMAPINFOHEADER));

	for(y = height-1; y >= 0; y--) {
		int i;
		for(i = 0, x = 0; x < width; x++) {
			uint32_t color = frame_buffer[x + y * texture_width];
			buffer[i+2] = (unsigned char)( color & 0xFF );
			buffer[i+1] = (unsigned char)( (color>>8) & 0xFF );
			buffer[i] = (unsigned char)( (color>>16) & 0xFF );
			i += 3;
		}
		sceIoWrite(fd, buffer, 3 * width);
	}
	sceIoClose(fd);

}

void clear_reset_framebuffer() {
	unsigned int texture_width;
	unsigned int clear_width;
	unsigned int clear_height;
	void *reset_surface;

	/* In direct-scanout mode ppa_gu_draw_buffer follows the current ring slot.
	 * A legacy 768x512 CPU memset from that address could run beyond the 2 MiB
	 * GE eDRAM aperture. Clear a known surface through the GE instead. */
	if (g_ppa_gu_direct_scanout) {
		reset_surface = ppa_gu_direct_scanout_surface(0);
		texture_width = PPA_GU_DIRECT_SCANOUT_PITCH;
		clear_width = 480U;
		clear_height = PPA_GU_DIRECT_SCANOUT_HEIGHT;
	}
	else if (g_ppa_gu_uses_extended_surfaces) {
		reset_surface = (void *)0x04000000U;
		texture_width = 768U;
		clear_width = 720U;
		clear_height = ppa_gu_scanout_storage_height();
	}
	else {
		reset_surface = (void *)0x04000000U;
		texture_width = 512U;
		clear_width = 480U;
		clear_height = 272U;
	}

	if (reset_surface == 0 ||
	    ppa_gu_clear_frame_blocking(reset_surface, texture_width,
	                                clear_width, clear_height) < 0)
		return;
	ppa_gu_draw_buffer = reset_surface;
	sceDisplayWaitVblankStart();
	sceDisplaySetFrameBuf(reset_surface, texture_width,
	                      PSP_DISPLAY_PIXEL_FORMAT_8888,
	                      PSP_DISPLAY_SETBUF_IMMEDIATE);
}

void make_screenshot() {
	int x, y, mode, pixel_format, width, height, texture_width;
	uint32_t* frame_buffer;
	unsigned char buffer[1440];
	char filename[512];
	FILE* fp;

	sceDisplayGetMode(&mode, &width, &height);
	if(width > 480)
		return;
	sceDisplayGetFrameBuf((void **)&frame_buffer, &texture_width, &pixel_format, 0);

	sceIoMkdir("ms0:/PICTURE", 0777);
	sceIoMkdir("ms0:/PICTURE/PPU", 0777);

	memset(filename, 0, 512);

	ScePspDateTime current_time;
	sceRtcGetCurrentClockLocalTime(&current_time);

	snprintf(filename, sizeof(filename), "ms0:/PICTURE/PPU/SNAPSHOT%04d%02d%02d%05d.PNG",
		current_time.year,
		current_time.month,
		current_time.day,
		current_time.hour*3600+current_time.minute*60+current_time.second);

	png_structp png_ptr = 0;
	png_infop info_ptr;

	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png_ptr)
		return;

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr) {
		png_destroy_write_struct(&png_ptr, (png_infopp)NULL);
		return;
	}

	fp = fopen(filename, "wb");
	if(fp == 0) {
		png_destroy_write_struct(&png_ptr, (png_infopp)NULL);
		return;
	}

	png_init_io(png_ptr, fp);
	png_set_IHDR(png_ptr, info_ptr, width, height,
		8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png_ptr, info_ptr);

	for(y = 0; y < height; y++) {
		int i;
		for(i = 0, x = 0; x < width; x++) {
			uint32_t color = frame_buffer[x + y * texture_width];
			buffer[i++] = (unsigned char)( color & 0xFF );
			buffer[i++] = (unsigned char)( (color>>8) & 0xFF );
			buffer[i++] = (unsigned char)( (color>>16) & 0xFF );
		}
		png_write_row(png_ptr, buffer);
	}
	png_write_end(png_ptr, info_ptr);
	png_destroy_write_struct(&png_ptr, (png_infopp)NULL);
	fclose(fp);
}
