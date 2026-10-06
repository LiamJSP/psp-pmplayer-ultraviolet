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
 
#ifndef __MP4INFO_TYPE_H__
#define __MP4INFO_TYPE_H__

#include <stdint.h>
#include <pspiofilemgr.h>

#define MP4INFO_MAX_METADATA_BYTES (8U * 1024U * 1024U)

#define MP4_MAX_TRACKS  1024
#define MP4_MAX_CHAPTERS 128
#define MP4_MAX_CHAPTER_TITLE 192
#define MP4_MAX_CHAPTER_REFS 8
#define MP4_TRACK_UNKNOWN  0
#define MP4_TRACK_AUDIO    1
#define MP4_TRACK_VIDEO    2
#define MP4_TRACK_SUBTITLE 3
#define MP4_TRACK_SYSTEM   4

typedef struct {
	uint64_t time_ms;
	char title[MP4_MAX_CHAPTER_TITLE];
} mp4info_chapter_t;

typedef struct {
	uint32_t type;
	uint32_t track_id;
	uint32_t sample_entry_type;
	uint32_t sample_tables_seen; /* duplicate tables must not overwrite owned arrays */
	uint32_t chapter_ref_count;
	uint32_t chapter_ref_ids[MP4_MAX_CHAPTER_REFS];
	uint32_t video_type;
	uint32_t audio_type;
	uint32_t audio_object_type;
	
	uint32_t time_scale;
	uint32_t duration;
	
	uint32_t width;
	uint32_t height;
	
	uint32_t channels;
	uint32_t samplerate;
	uint32_t samplebits;
	/* Bounded FLAC STREAMINFO / Opus dOps, retained without heap ownership. */
	uint8_t audio_private[42];
	uint32_t audio_private_size;
	/* New software codecs accept one nonempty, unit-rate edit only. Complex
	 * edits are explicitly inadmissible, never silently ignored for Opus. */
	int32_t audio_edit_status; /* 0 absent, 1 supported, -1 unsupported */
	uint64_t audio_edit_start;
	uint64_t audio_edit_duration;
	
	uint32_t stts_entry_count;
	uint32_t *stts_sample_count;
	uint32_t *stts_sample_duration;
	
	uint32_t ctts_entry_count;
	uint32_t ctts_version;
	uint32_t *ctts_sample_count;
	int32_t *ctts_sample_offset;
	
	uint32_t stss_entry_count;
	uint32_t *stss_sync_sample;
	
	uint32_t stsc_entry_count;
	uint32_t *stsc_first_chunk;
	uint32_t *stsc_samples_per_chunk;
	uint32_t *stsc_sample_desc_id;
	
	uint32_t stsz_sample_size;
	uint32_t stsz_entry_count;
	uint32_t *stsz_sample_size_table;
	
	uint32_t stco_entry_count;
	uint64_t *stco_chunk_offset;
	
	uint32_t avc_profile;
	uint32_t avc_sps_size;
	uint8_t* avc_sps;
	uint32_t avc_pps_size;
	uint8_t* avc_pps;
	uint32_t avc_nal_prefix_size;
	
	uint32_t mp4a_is_sbr;
	uint32_t mp4a_sbr_samplerate;
	
} mp4info_track_t;

typedef struct {
	void* handle;
	
	int32_t time_scale;
	int32_t duration;
	uint64_t file_size;
	uint32_t metadata_allocated_bytes; /* cumulative bounded parser allocation */
	int parse_error;

	int32_t total_tracks;
	mp4info_track_t* tracks[MP4_MAX_TRACKS];

	uint32_t chapter_count;
	mp4info_chapter_t chapters[MP4_MAX_CHAPTERS];
	
} mp4info_t;

typedef struct {
	void* handle;
	char* title;
	char* artist;
	char* album;
} mp4meta_t;

#endif
