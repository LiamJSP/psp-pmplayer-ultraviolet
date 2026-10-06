/* 
 *	Copyright (C) 2008 cooleyes
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
 
#ifndef __MP4_FILE_H__
#define __MP4_FILE_H__

#include <stdio.h>
#include "audio_format.h"
#include <stdint.h>
#include <string.h>
#include "mp4info.h"
#include "common/mem64.h"
#include "../common/ppa_chapter.h"

struct mp4_sample_struct {
	uint32_t sample_index;
	uint32_t sample_size;
	uint64_t file_offset;
};

struct mp4_index_struct {
	unsigned int timestamp;
	unsigned int sample_index;
	uint64_t offset;
};

struct mp4_file_struct {
	mp4info_t *info;
	int video_track_id;
	int audio_tracks;
	int audio_track_ids[6];
	struct ppu_audio_format audio_formats[6];
	int audio_battery_cbr[6];
	int subtitle_tracks;
	int subtitle_track_ids[4];
	
	int subtitle_track_time_count[4];
	unsigned int *subtitle_track_time[4];
	unsigned int subtitle_track_time_scale[4];
	
	unsigned int video_type;
	unsigned int video_width;
	unsigned int video_height;
	unsigned int number_of_video_frames;
	unsigned int video_rate;
	unsigned int video_scale;
	uint32_t video_stts_entry_count;
	uint32_t *video_stts_sample_count;
	uint32_t *video_stts_sample_duration;
	uint32_t video_ctts_entry_count;
	uint32_t *video_ctts_sample_count;
	int32_t *video_ctts_sample_offset;
	
	unsigned int maximum_video_sample_size;
	
	unsigned int avc_profile;
	unsigned int avc_sps_size;
	unsigned char* avc_sps;
	unsigned int avc_pps_size;
	unsigned char* avc_pps;
	unsigned int avc_nal_prefix_size;

	unsigned int audio_type;
	unsigned int audio_actual_rate;
	unsigned int audio_rate;
	unsigned int audio_resample_scale;
	unsigned int audio_resample_num;
	unsigned int audio_resample_den;
	unsigned int audio_scale;
	unsigned int audio_stereo;
	unsigned int audio_channels;
	/* Compact container clock runs, indexed by selectable audio slot.
	 * Ownership transfers from mp4info after admission; no per-packet table. */
	uint32_t audio_time_scale[6];
	uint32_t audio_sample_count[6];
	uint32_t audio_stts_entry_count[6];
	uint32_t *audio_stts_sample_count[6];
	uint32_t *audio_stts_sample_duration[6];
    uint32_t audio_stts_cursor[6];
    uint64_t audio_stts_cursor_sample[6], audio_stts_cursor_ticks[6];
	
	int audio_up_sample;
	
	
	int seek_duration;
	unsigned int duration_ms;
	unsigned int chapter_count;
	struct ppa_chapter chapters[PPA_MAX_CHAPTERS];
	
	unsigned int sample_count;
	struct mp4_sample_struct* samples;
	unsigned int index_count;
	struct mp4_index_struct* indexes;
	
	int is_not_interlace;
	uint64_t first_video_offset;
	unsigned int first_video_sample;
	uint64_t first_audio_offset;
	unsigned int first_audio_sample;
	
};

uint64_t mp4_file_audio_sample_position(struct mp4_file_struct *p, unsigned int slot, unsigned int sample);
uint32_t mp4_file_audio_sample_duration(struct mp4_file_struct *p, unsigned int slot, unsigned int sample);

void mp4_file_safe_constructor(struct mp4_file_struct *p);
void mp4_file_close(struct mp4_file_struct *p);
char *mp4_file_open(struct mp4_file_struct *p, char *s);
unsigned int mp4_file_video_timestamp_ms(const struct mp4_file_struct *p,
                                         unsigned int sample_index);
unsigned int mp4_file_audio_timestamp_ms(struct mp4_file_struct *p,
                                         unsigned int audio_slot,
                                         unsigned int sample_index);
unsigned int mp4_file_audio_sample_at_ms(const struct mp4_file_struct *p,
                                        unsigned int audio_slot,
                                        unsigned int timestamp);

#endif
