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
#ifndef __MKV_DECODE_H__
#define __MKV_DECODE_H__

#include <pspkernel.h>
#include <pspdisplay.h>
#include "mkv_read.h"
#include "audiodecoder.h"
#include "audio_stream.h"
#include "common/mem64.h"
#include "common/m33sdk.h"
#include "aspect_ratio.h"
#include "gu_draw.h"
#include "movie_interface.h"
#include "mp4avcdecoder.h"
#include "cpu_clock.h"
#include "h264x_probe.h"
#include "h264x_rewrite.h"

#include "subtitle_parse.h"
#include "gu_font.h"

#define mkv_maximum_frame_buffers 64
#define mkv_number_of_free_video_frame_buffers 6
#define mkv_timestamp_queue_max 16

/* Output identity travels with each frame so presentation can restore B-frame
 * order. Audio buffers leave these fields zero. */
struct mkv_decode_buffer_struct {
	void *data;
	int timestamp;
	unsigned int epoch;
	unsigned int h264x_valid;
	unsigned int h264x_source_kind;
	unsigned int h264x_native_output_seq;
	unsigned int h264x_au_index;
	unsigned int h264x_poc_lsb;
	unsigned int h264x_is_b;
	unsigned int h264x_is_reference_b;
	unsigned int h264x_is_p;
	unsigned int h264x_risk;
	unsigned int h264x_risk_class;
	unsigned int h264x_rplm_found;
	unsigned int h264x_mmco_found;
};

/* Sink decision values are part of the cross-translation-unit API. */
#define MKV_H264X_SINK_DECISION_HOLD    0U
#define MKV_H264X_SINK_DECISION_DISPLAY 1U
#define MKV_H264X_SINK_DECISION_SKIP    2U

struct mkv_h264x_present_risk_struct {
	int valid;
	int timestamp;
	unsigned int au_index;
	unsigned int au_size;
	unsigned int frame_num;
	unsigned int poc_lsb;
	unsigned int slice_type;
	unsigned int nal_ref_idc;
	int is_i;
	int is_p;
	int is_b;
	int is_reference_b;
	unsigned int risk;
	unsigned int risk_class;
	unsigned int rplm_found;
	unsigned int rplm_missing;
	unsigned int mmco_found;
	unsigned int mmco_absent;
	unsigned int true_missing_total;
	unsigned int absent_total;
	unsigned int benign_absent_total;
	
};

struct mkv_decode_struct {
	unsigned int h264x_output_sequence;
	struct mkv_read_struct reader;

	struct mp4_avc_struct avc;
	
	unsigned int video_format;

	int audio_decoder;
	struct ppu_audio_stream *audio_stream;
	unsigned int audio_stream_selected;

	void *video_frame_buffers[mkv_maximum_frame_buffers];
	void *audio_frame_buffers[mkv_maximum_frame_buffers];
	
	unsigned int video_frame_size;
	unsigned int audio_frame_size;
	unsigned int number_of_frame_buffers;

	struct mkv_decode_buffer_struct output_audio_frame_buffers[mkv_maximum_frame_buffers];
	struct mkv_decode_buffer_struct output_video_frame_buffers[mkv_maximum_frame_buffers];

	unsigned int current_audio_buffer_number;
	unsigned int current_video_buffer_number;
	unsigned int output_epoch;
	
	int output_texture_width;
	int video_frame_duration;
	int audio_frame_duration;
	
	int timestamp_queue[mkv_timestamp_queue_max];
	unsigned int timestamp_queue_size;
	struct mkv_h264x_present_risk_struct h264x_present_queue[mkv_timestamp_queue_max];
	unsigned int h264x_present_queue_size;

	int last_audio_timestamp;
	int last_video_timestamp;

	unsigned int avc_first_packet_done;

	/* 6.3.39 decoder-facing compatibility normalization. The original MKV
	 * bitstream metadata remains in h264x_probe; only the SPS/PPS and AU copy
	 * submitted to Sony are normalized when the observed hardware envelope
	 * would otherwise reject the first IDR. */
	unsigned int h264x_compat_active;
	unsigned int h264x_compat_flags;
	unsigned int h264x_compat_original_refs;
	unsigned int h264x_compat_effective_refs;
	unsigned int h264x_compat_force_level_idc;
	unsigned int h264x_compat_mpeg_mode;
	unsigned int h264x_compat_use_original_ps;

	unsigned int h264x_compat_disable_weights;
	unsigned int h264x_compat_flatten_brefs;
	unsigned int h264x_compat_retry_stage;
	unsigned int h264x_compat_generic_burst_reverse;

	unsigned int h264x_compat_first_idr_retry_attempted;

	unsigned int h264x_compat_pic_max;

	struct h264x_probe h264x;

	int is_eof;
	
};

void  mkv_decode_safe_constructor(struct mkv_decode_struct *p);
char *mkv_decode_open(struct mkv_decode_struct *p, char *s, int pspType, int tvAspectRatio, int tvWidth, int tvHeight, int videoMode);
void  mkv_decode_close(struct mkv_decode_struct *p, int pspType);
int   mkv_decode_is_eof(struct mkv_decode_struct *p);
void  mkv_decode_reset(struct mkv_decode_struct *p);
char *mkv_decode_seek(struct mkv_decode_struct *p, int timestamp, int last_timestamp);
char *mkv_decode_get_video(struct mkv_decode_struct *p, unsigned int audio_stream, unsigned int volume_boost, unsigned int aspect_ratio, unsigned int zoom, unsigned int luminosity_boost, unsigned int show_interface, unsigned int show_subtitle, unsigned int subtitle_format, unsigned int loop, int* pic_num );
char *mkv_decode_get_cached_video(struct mkv_decode_struct *p, unsigned int pic_num, unsigned int audio_stream, unsigned int volume_boost, unsigned int aspect_ratio, unsigned int zoom, unsigned int luminosity_boost, unsigned int show_interface, unsigned int show_subtitle, unsigned int subtitle_format, unsigned int loop);
char *mkv_decode_get_audio(struct mkv_decode_struct *p, unsigned int audio_stream, int audio_channel, int decode_audio, unsigned int volume_boost, unsigned int *codec_us);

/* Schema-independent presentation/sink telemetry entry point. */

#define MKV_H264X_DIAG_REASON_DECODE_ERROR 1U
#define MKV_H264X_DIAG_REASON_WATCHDOG    2U
#define MKV_H264X_DIAG_REASON_INVARIANT   3U

#endif
