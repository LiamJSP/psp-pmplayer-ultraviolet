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

#ifndef __MKV_READ_H__
#define __MKV_READ_H__

#include <string.h>
#include <pspiofilemgr.h>
#include "mkv_file.h"
#include "common/mem64.h"
#include "common/buffered_reader.h"
#include "../common/ppa_media_packet.h"
#include "../common/ppa_io_pump.h"
#include "../common/ppa_queue_watermark.h"

#define MKV_READ_EOF "mkv_read_fill_buffer: eof"
#define MKV_VIDEO_QUEUE_MAX 64
#define MKV_AUDIO_QUEUE_MAX 64

/* A Matroska scan can encounter interleaved/laced packets for the opposite
 * stream before the requested block. Stop before consuming another block once
 * that queue reaches its time/byte target. This is flow control, not an error. */
#define MKV_READ_VIDEO_AUDIO_BACKPRESSURE \
    "mkv_read_get_video: audio queue backpressure"
#define MKV_READ_AUDIO_VIDEO_BACKPRESSURE \
    "mkv_read_get_audio: video queue backpressure"

/* Legacy public tag retained as a source-compatible alias. */
#define mkv_read_output_struct ppa_media_packet

void mkv_read_release_output(struct mkv_read_output_struct *p);

struct mkv_read_struct {
	struct mkv_file_struct file;
	
	buffered_reader_t* reader;
	struct PpaIoPump *io_pump;
	
	int current_audio_track;
	
	uint64_t cluster_size;
	uint64_t cluster_timecode;
	uint64_t blockgroup_size;
	uint64_t block_size;
	
	void* block_buffer;
	
	struct mkv_read_output_struct video_queue[MKV_VIDEO_QUEUE_MAX];
	struct mkv_read_output_struct audio_queue[MKV_AUDIO_QUEUE_MAX];
	unsigned int video_queue_front, video_queue_rear, video_queue_size;
	unsigned int audio_queue_front, audio_queue_rear, audio_queue_size;
	unsigned int block_buffer_capacity;
	struct PpaQueueWatermark video_watermark;
	struct PpaQueueWatermark audio_watermark;
	unsigned int video_trim_floor;
	unsigned int audio_trim_floor;
};

void mkv_read_safe_constructor(struct mkv_read_struct *p);
void mkv_read_close(struct mkv_read_struct *p);
char *mkv_read_open(struct mkv_read_struct *p, char *s);
char *mkv_read_seek(struct mkv_read_struct *p, int timestamp, int last_timestamp);
char *mkv_read_get_video(struct mkv_read_struct *p, struct mkv_read_output_struct *output);
char *mkv_read_get_audio(struct mkv_read_struct *p, unsigned int audio_stream, struct mkv_read_output_struct *output);
void mkv_read_set_video_reorder_requirement(struct mkv_read_struct *p,
                                             unsigned int reorder_frames);

#endif
