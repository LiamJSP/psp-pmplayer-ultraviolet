/*
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

#ifndef gu_draw_h
#define gu_draw_h

#define number_of_luminosity_boosts 16

#define PPA_GU_DIRECT_SCANOUT_COUNT 3U
#define PPA_GU_DIRECT_SCANOUT_PITCH 512U
#define PPA_GU_DIRECT_SCANOUT_HEIGHT 272U
#define PPA_GU_DIRECT_SCANOUT_BYTES \
	(PPA_GU_DIRECT_SCANOUT_PITCH * PPA_GU_DIRECT_SCANOUT_HEIGHT * 4U)

#include <string.h>
#include <pspkernel.h>
#include <pspgu.h>
#include "aspect_ratio.h"
#include "common/texture_subdivision.h"
#include "common/m33sdk.h"
#include "movie_interface.h"
#include "subtitle_parse.h"
#include "gu_font.h"

struct vertex_struct
	{
	short texture_x;
	short texture_y;

	float vertex_x;
	float vertex_y;
	float vertex_z;
	};

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned int __attribute__((aligned(16))) ppa_gu_list[262144];

extern void *ppa_gu_draw_buffer;
extern void *ppa_gu_rgb_buffer;

void ppa_gu_init_previous_values();
void ppa_gu_start(int psp_type, int tv_aspectratio, int tv_overscan_left, int tv_overscan_top, int tv_overscan_right, int tv_overscan_bottom, int video_mode);
void ppa_gu_end();
void ppa_gu_wait();
int ppa_gu_direct_scanout_enabled(void);
void *ppa_gu_direct_scanout_surface(unsigned int index);
int ppa_gu_clear_frame_blocking(void *destination,
                                unsigned int destination_pitch,
                                unsigned int width,
                                unsigned int height);
unsigned int ppa_gu_scanout_width(void);
unsigned int ppa_gu_scanout_height(void);
/* Interlaced TV stores two fields with a 262-row offset. */
unsigned int ppa_gu_scanout_storage_height(void);
int ppa_gu_scanout_interlaced(void);
void ppa_gu_output_rect(unsigned int *left, unsigned int *top,
                        unsigned int *width, unsigned int *height);
int ppa_gu_copy_frame_blocking(void *destination,
                               unsigned int destination_pitch,
                               const void *source,
                               unsigned int source_pitch,
                               unsigned int width,
                               unsigned int height);
void ppa_gu_draw(unsigned int aspect_ratio, unsigned int zoom, unsigned int luminosity_boost, unsigned int show_interface, unsigned int show_subtitle, unsigned int subtitle_format, unsigned int frame_number, void *video_frame_buffer);
void ppa_gu_draw_video_only(unsigned int aspect_ratio, unsigned int zoom, unsigned int luminosity_boost, unsigned int subtitle_format, unsigned int frame_number, void *video_frame_buffer);
void ppa_gu_overlay_cpu_frame(unsigned int show_interface,
                              unsigned int show_subtitle,
                              unsigned int subtitle_format,
                              unsigned int frame_number,
                              void *video_frame_buffer,
                              unsigned int output_texture_width);
unsigned int ppa_gu_text_panel_backup_bytes(void);
int ppa_gu_capture_text_panel_region(void *backup, unsigned int bytes);
int ppa_gu_restore_text_panel_region(const void *backup, unsigned int bytes);
int ppa_gu_draw_text_panel_blocking(const char *title,
                                    const char *body,
                                    const char *footer);
int ppa_gu_draw_recovery_text_panel_blocking(const char *title,
                                             const char *body,
                                             const char *footer);

void make_screenshot();
void clear_reset_framebuffer();

#ifdef __cplusplus
}
#endif

#endif
