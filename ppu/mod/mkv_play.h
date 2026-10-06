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
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */
#ifndef __MKV_PLAY_H__
#define __MKV_PLAY_H__

#include <pspthreadman.h>
#include <pspaudio.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psppower.h>
#include "mkv_decode.h"
#include "aspect_ratio.h"
#include "gu_draw.h"
#include "gu_util.h"
#include "subtitle_parse.h"
#include "movie_file.h"
#include "../common/ppa_seek_controller.h"

struct mkv_play_struct {
    volatile int audio_tail_done;

	struct mkv_decode_struct decoder;

	int audio_reserved;

	SceUID semaphore_can_put_video;
	SceUID semaphore_can_put_audio;
	SceUID semaphore_can_get_video;
	SceUID semaphore_can_get_audio;

	SceUID output_thread;
	SceUID show_thread;
	SceUID demux_thread;

	int   return_request;
	char *return_result;

	int paused;
	int seek;
	int absolute_seek_ms;
	int chapter_skip_cursor;
	uint64_t chapter_skip_deadline_us;
	struct ppa_seek_controller seek_controller;
	struct ppu_seek_request seek_request;
	uint64_t seek_session_deadline_us;
	unsigned int seek_transactions;
	unsigned int seek_failures;
	unsigned int trickplay_frame_counter;
	unsigned int trickplay_settle_pending;
	volatile unsigned int seek_in_flight;
	/* Producer stops at the first decoded audio buffer at/after the target. */
	volatile unsigned int seek_audio_target_queued;
	unsigned int seek_in_flight_epoch;
	int seek_in_flight_target_ms;
	unsigned int h264x_trickplay_saved_mode;

	unsigned int audio_stream;
	int audio_channel;
	unsigned int volume_boost;
	unsigned int aspect_ratio;
	unsigned int zoom;
	unsigned int luminosity_boost;
	unsigned int show_interface;
	uint64_t interface_hide_deadline_us;
	unsigned int last_keyframe_pos;
	unsigned int resume_pos;
	int persistent_state_ready;
	char hash[16];
	char movie_file[512];
	unsigned int subtitle_count;
	unsigned int subtitle;
	unsigned int subtitle_format;
	unsigned int subtitle_fontcolor;
	unsigned int subtitle_bordercolor;
	volatile int subtitle_font_reload_pending;
	unsigned int loop;
	
	int current_video_buffer_number;
	int current_audio_buffer_number;
	
	int current_timestamp;

	unsigned int h264x_playback_mode;
	/* Presentation stays locked to mode 5. The decoder remap selection remains
	 * separate from the 6.3.35 missing-sample restoration selector below. */
	unsigned int h264x_output_remap_policy;

	/* 6.3.37 production restoration. Policy 1 performs strict native triplet
	 * rotation first, then AV-budgeted midpoint generation, then hard
	 * suppression. Policy 0 remains the verified artifact-free emergency
	 * control; all policies preserve immutable staging/NEXTFRAME. */
	unsigned int h264x_restore_policy;

	void *h264x_restore_saved_surface;
	void *h264x_restore_generated_surface;
	unsigned int h264x_restore_surface_bytes;
	unsigned int h264x_restore_swap_pending;
	unsigned int h264x_restore_swap_policy;
	unsigned int h264x_restore_pair_next_id;
	unsigned int h264x_restore_swap_pair_id;

	unsigned int h264x_restore_swap_target_native_seq;
	unsigned int h264x_restore_swap_target_poc_lsb;
	int h264x_restore_swap_target_timestamp;

	void *h264x_present_anchor_history;
	unsigned int h264x_present_anchor_history_bytes;
	unsigned int h264x_present_anchor_history_valid;
	int h264x_present_anchor_history_timestamp;

	

	

	/* Two immutable presentation surfaces isolate scanout/reuse/cache races. */
	void *h264x_stage_surface[2];
	unsigned int h264x_stage_surface_bytes;
	unsigned int h264x_stage_surface_index;

	/* Fixed cadence modes use a bounded 29.97-fps timer.  Never block in
	 * sceDisplayWaitVblankStart(): mode switches and shutdown must stay live. */
	unsigned int h264x_fixed_cadence_valid;
	unsigned int h264x_fixed_cadence_last_mode;
	unsigned int h264x_fixed_cadence_next_tick;

};

#ifndef NUMBER_OF_FONTCOLORS
#define NUMBER_OF_FONTCOLORS 6
#endif
#ifndef NUMBER_OF_BORDERCOLORS
#define NUMBER_OF_BORDERCOLORS 6
#endif

#include "movie_stat.h"
#ifdef __cplusplus
extern "C" {
#endif

void mkv_play_safe_constructor(struct mkv_play_struct *p);
char *mkv_play_open(struct mkv_play_struct *p, struct movie_file_struct *movie, int usePos, int pspType, int tvAspectRatio, int tvWidth, int tvHeight, int videoMode);
void mkv_play_close(struct mkv_play_struct *p, int usePos, int pspType);
char *mkv_play_start(volatile struct mkv_play_struct *p);

#ifdef __cplusplus
}
#endif

#endif
