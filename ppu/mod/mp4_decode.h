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
#ifndef __MP4_DECODE_H__
#define __MP4_DECODE_H__

#include <pspkernel.h>
#include <pspdisplay.h>
#include "mp4_read.h"
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
#include "h264x_compat.h"

#include "subtitle_parse.h"
#include "gu_font.h"

#define mp4_maximum_frame_buffers 64
#define mp4_number_of_free_video_frame_buffers 6
#define mp4_timestamp_queue_max 16

struct mp4_decode_buffer_struct {
	void *data;
	int timestamp;
	unsigned int epoch;
};

struct mp4_decode_struct {
	struct mp4_read_struct reader;

	struct mp4_avc_struct avc;
	
	unsigned int video_format;

	int audio_decoder;
	struct ppu_audio_stream *audio_stream;
	unsigned int audio_stream_selected;

	void *video_frame_buffers[mp4_maximum_frame_buffers];
	void *audio_frame_buffers[mp4_maximum_frame_buffers];
	
	unsigned int video_frame_size;
	unsigned int audio_frame_size;
	unsigned int number_of_frame_buffers;

	struct mp4_decode_buffer_struct output_audio_frame_buffers[mp4_maximum_frame_buffers];
	struct mp4_decode_buffer_struct output_video_frame_buffers[mp4_maximum_frame_buffers];

	unsigned int current_audio_buffer_number;
	unsigned int current_video_buffer_number;
	
	int output_texture_width;
	int video_frame_duration;
	int audio_frame_duration;
	
	int timestamp_queue[mp4_timestamp_queue_max];
	unsigned int timestamp_queue_size;
	struct h264x_picture_identity h264x_present_queue[mp4_timestamp_queue_max];
	struct h264x_two_picture_remap h264x_output_remap;
	unsigned int h264x_deep_b_detected;
	unsigned int h264x_anchor_valid, h264x_anchor_poc_lsb;
	
	int last_audio_timestamp;
	int last_video_timestamp;
	
	int is_eof;
	int avc_first_packet_done;
	unsigned int output_epoch;

	/* MP4 uses the same bounded decoder-compatibility primitives as MKV.
	 * The probe parses SPS/PPS and AU headers; the rewrite buffer is allocated
	 * once from the validated maximum sample size. */
	struct h264x_probe h264x;
	void *h264x_rewrite_buffer;
	unsigned int h264x_rewrite_capacity;
	unsigned int h264x_compat_active;
	unsigned int h264x_compat_original_refs;
	unsigned int h264x_compat_effective_refs;
	unsigned int h264x_compat_disable_weights;
	unsigned int h264x_compat_flatten_brefs;
	unsigned int h264x_compat_force_level_idc;
	unsigned int h264x_compat_mpeg_mode;
	unsigned int h264x_compat_retry_stage;
	unsigned int h264x_compat_first_idr_retry_attempted;

};

void  mp4_decode_safe_constructor(struct mp4_decode_struct *p);
char *mp4_decode_open(struct mp4_decode_struct *p, char *s, int pspType, int tvAspectRatio, int tvWidth, int tvHeight, int videoMode);
void  mp4_decode_close(struct mp4_decode_struct *p, int pspType);
int   mp4_decode_is_eof(struct mp4_decode_struct *p);
void  mp4_decode_reset(struct mp4_decode_struct *p);
char *mp4_decode_seek(struct mp4_decode_struct *p, int timestamp, int last_timestamp);
char *mp4_decode_get_video(struct mp4_decode_struct *p, unsigned int audio_stream, unsigned int volume_boost, unsigned int aspect_ratio, unsigned int zoom, unsigned int luminosity_boost, unsigned int show_interface, unsigned int show_subtitle, unsigned int subtitle_format, unsigned int loop, int* pic_num );
char *mp4_decode_get_cached_video(struct mp4_decode_struct *p, unsigned int pic_num, unsigned int audio_stream, unsigned int volume_boost, unsigned int aspect_ratio, unsigned int zoom, unsigned int luminosity_boost, unsigned int show_interface, unsigned int show_subtitle, unsigned int subtitle_format, unsigned int loop);
char *mp4_decode_get_audio(struct mp4_decode_struct *p, unsigned int audio_stream, int audio_channel, int decode_audio, unsigned int volume_boost, unsigned int *codec_us);
char *mp4_decode_keyframe_forward(struct mp4_decode_struct *p, int keyframes);
char *mp4_decode_keyframe_backward(struct mp4_decode_struct *p, int keyframes);

#endif
