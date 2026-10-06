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
 
#ifndef __MKV_FILE_H__
#define __MKV_FILE_H__

#include <stdio.h>
#include "audio_format.h"
#include "mkvinfo.h"
#include "subtitle_parse.h"
#include "../common/ppa_chapter.h"

struct mkv_file_struct {
	mkvinfo_t *info;
	int video_track_id;
	int audio_tracks;
	int audio_track_ids[6];
	struct ppu_audio_format audio_formats[6];
	int subtitle_tracks;
	int subtitle_track_ids[MAX_SUBTITLES];
	uint32_t subtitle_track_types[MAX_SUBTITLES];
	
	unsigned int video_type;
	unsigned int video_width;
	unsigned int video_height;
	unsigned int display_width;
	unsigned int display_height;
	unsigned int video_rate;
	unsigned int video_scale;
	unsigned int number_of_video_frames;
	
	unsigned int audio_type;
	unsigned int audio_actual_rate;
	unsigned int audio_rate;
	unsigned int audio_resample_scale;
	unsigned int audio_resample_num;
	unsigned int audio_resample_den;
	unsigned int audio_scale;
	unsigned int audio_stereo;
	unsigned int audio_channels;
	
	int audio_up_sample;
	
	int seek_duration;
	unsigned int duration_ms;
	unsigned int chapter_count;
	struct ppa_chapter chapters[PPA_MAX_CHAPTERS];
};

void mkv_file_safe_constructor(struct mkv_file_struct *p);
void mkv_file_close(struct mkv_file_struct *p);
char *mkv_file_open(struct mkv_file_struct *p, char *s);

#endif

