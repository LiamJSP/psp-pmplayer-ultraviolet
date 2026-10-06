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
 
#include "atom.h"
#include "bufferedio.h"
#include "mp4info.h"
#include<stdio.h>
#include<stdlib.h>
#include<string.h>


#define MP4_CHAPTER_SAMPLE_MAX 4096U

static uint32_t mp4info_sample_size(const mp4info_track_t *track,
                                    uint32_t sample)
{
    if (track == 0 || sample >= track->stsz_entry_count)
        return 0;
    if (track->stsz_sample_size != 0)
        return track->stsz_sample_size;
    if (track->stsz_sample_size_table == 0)
        return 0;
    return track->stsz_sample_size_table[sample];
}

static uint32_t mp4info_utf8_emit(char *dst, uint32_t cap,
                                  uint32_t out, uint32_t cp)
{
    if (cp == 0 || cp > 0x10ffffU || (cp >= 0xd800U && cp <= 0xdfffU))
        cp = 0x25a1U;
    if (cp < 0x80U) {
        if (out + 1U < cap) dst[out++] = (char)cp;
    }
    else if (cp < 0x800U) {
        if (out + 2U < cap) {
            dst[out++] = (char)(0xc0U | (cp >> 6));
            dst[out++] = (char)(0x80U | (cp & 0x3fU));
        }
    }
    else if (cp < 0x10000U) {
        if (out + 3U < cap) {
            dst[out++] = (char)(0xe0U | (cp >> 12));
            dst[out++] = (char)(0x80U | ((cp >> 6) & 0x3fU));
            dst[out++] = (char)(0x80U | (cp & 0x3fU));
        }
    }
    else if (out + 4U < cap) {
        dst[out++] = (char)(0xf0U | (cp >> 18));
        dst[out++] = (char)(0x80U | ((cp >> 12) & 0x3fU));
        dst[out++] = (char)(0x80U | ((cp >> 6) & 0x3fU));
        dst[out++] = (char)(0x80U | (cp & 0x3fU));
    }
    return out;
}

static void mp4info_sanitize_utf8(const uint8_t *src, uint32_t size,
                                  char *dst, uint32_t cap)
{
    uint32_t i = 0;
    uint32_t out = 0;
    if (dst == 0 || cap == 0)
        return;
    while (i < size && src[i] != 0 && out + 1U < cap) {
        uint32_t cp;
        uint32_t need;
        uint8_t b = src[i];
        if (b < 0x80U) {
            cp = b;
            need = 1;
        }
        else if ((b & 0xe0U) == 0xc0U) {
            cp = b & 0x1fU; need = 2;
        }
        else if ((b & 0xf0U) == 0xe0U) {
            cp = b & 0x0fU; need = 3;
        }
        else if ((b & 0xf8U) == 0xf0U) {
            cp = b & 0x07U; need = 4;
        }
        else {
            out = mp4info_utf8_emit(dst, cap, out, 0x25a1U);
            i++;
            continue;
        }
        if (i + need > size) {
            out = mp4info_utf8_emit(dst, cap, out, 0x25a1U);
            break;
        }
        {
            uint32_t j;
            int valid = 1;
            for (j = 1; j < need; ++j) {
                if ((src[i + j] & 0xc0U) != 0x80U) { valid = 0; break; }
                cp = (cp << 6) | (src[i + j] & 0x3fU);
            }
            if (!valid || (need == 2 && cp < 0x80U) ||
                (need == 3 && cp < 0x800U) ||
                (need == 4 && cp < 0x10000U)) {
                out = mp4info_utf8_emit(dst, cap, out, 0x25a1U);
                i++;
                continue;
            }
        }
        out = mp4info_utf8_emit(dst, cap, out, cp);
        i += need;
    }
    dst[out] = 0;
}

static void mp4info_decode_chapter_text(const uint8_t *data, uint32_t size,
                                        char *dst, uint32_t cap)
{
    uint32_t offset = 0;
    uint32_t text_size = size;
    uint32_t out = 0;
    if (dst == 0 || cap == 0) return;
    dst[0] = 0;
    if (data == 0 || size == 0) return;
    if (size >= 2) {
        uint32_t declared = ((uint32_t)data[0] << 8) | data[1];
        if (declared <= size - 2U) {
            offset = 2;
            text_size = declared;
        }
    }
    while (text_size > 0 && data[offset + text_size - 1U] == 0)
        text_size--;
    if (text_size >= 2 &&
        ((data[offset] == 0xfe && data[offset + 1] == 0xff) ||
         (data[offset] == 0xff && data[offset + 1] == 0xfe))) {
        int little = data[offset] == 0xff;
        uint32_t i = offset + 2U;
        uint32_t end = offset + text_size;
        while (i + 1U < end && out + 1U < cap) {
            uint32_t w1 = little ?
                ((uint32_t)data[i] | ((uint32_t)data[i + 1] << 8)) :
                (((uint32_t)data[i] << 8) | data[i + 1]);
            uint32_t cp = w1;
            i += 2;
            if (w1 >= 0xd800U && w1 <= 0xdbffU && i + 1U < end) {
                uint32_t w2 = little ?
                    ((uint32_t)data[i] | ((uint32_t)data[i + 1] << 8)) :
                    (((uint32_t)data[i] << 8) | data[i + 1]);
                if (w2 >= 0xdc00U && w2 <= 0xdfffU) {
                    cp = 0x10000U + ((w1 - 0xd800U) << 10) + (w2 - 0xdc00U);
                    i += 2;
                }
                else cp = 0x25a1U;
            }
            out = mp4info_utf8_emit(dst, cap, out, cp);
        }
        dst[out] = 0;
    }
    else {
        mp4info_sanitize_utf8(data + offset, text_size, dst, cap);
    }
}

static mp4info_track_t *mp4info_find_track_id(mp4info_t *info, uint32_t id)
{
    int32_t i;
    for (i = 0; i < info->total_tracks; ++i)
        if (info->tracks[i] && info->tracks[i]->track_id == id)
            return info->tracks[i];
    return 0;
}

static void mp4info_sort_chapters(mp4info_t *info)
{
    uint32_t i;
    if (info == 0) return;
    for (i = 1; i < info->chapter_count; ++i) {
        mp4info_chapter_t value = info->chapters[i];
        uint32_t j = i;
        while (j > 0 && info->chapters[j - 1U].time_ms > value.time_ms) {
            info->chapters[j] = info->chapters[j - 1U];
            --j;
        }
        info->chapters[j] = value;
    }
    if (info->chapter_count > 1U) {
        uint32_t out = 1U;
        for (i = 1U; i < info->chapter_count; ++i) {
            if (info->chapters[i].time_ms == info->chapters[out - 1U].time_ms)
                continue;
            if (out != i) info->chapters[out] = info->chapters[i];
            ++out;
        }
        info->chapter_count = out;
    }
}

static void mp4info_extract_quicktime_chapters(mp4info_t *info)
{
    mp4info_track_t *chapter_track = 0;
    int32_t i;
    uint32_t chunk;
    uint32_t sample = 0;
    uint32_t stsc = 0;
    uint32_t stts = 0;
    uint32_t stts_left = 0;
    uint64_t ticks = 0;
    uint8_t sample_buf[MP4_CHAPTER_SAMPLE_MAX];

    if (info == 0 || info->handle == 0)
        return;
    for (i = 0; i < info->total_tracks && chapter_track == 0; ++i) {
        mp4info_track_t *master = info->tracks[i];
        uint32_t r;
        if (master == 0) continue;
        for (r = 0; r < master->chapter_ref_count; ++r) {
            chapter_track = mp4info_find_track_id(info, master->chapter_ref_ids[r]);
            if (chapter_track != 0) break;
        }
    }
    /* A QuickTime chap track is metadata, not a selectable subtitle/media
     * stream. Mark it even when a parallel chpl list is preferred below. */
    if (chapter_track != 0)
        chapter_track->type = MP4_TRACK_SYSTEM;
    /* Do not merge two independent chapter representations into one list.
     * chpl is already complete and usually mirrors the referenced text track. */
    if (info->chapter_count != 0)
        return;
    if (chapter_track == 0 || chapter_track->time_scale == 0 ||
        chapter_track->stco_entry_count == 0 || chapter_track->stsc_entry_count == 0 ||
        chapter_track->stsz_entry_count == 0 || chapter_track->stts_entry_count == 0 ||
        chapter_track->stco_chunk_offset == 0 || chapter_track->stsc_first_chunk == 0 ||
        chapter_track->stsc_samples_per_chunk == 0 ||
        chapter_track->stts_sample_count == 0 || chapter_track->stts_sample_duration == 0)
        return;

    stts_left = chapter_track->stts_sample_count[0];
    for (chunk = 0; chunk < chapter_track->stco_entry_count &&
                    sample < chapter_track->stsz_entry_count &&
                    info->chapter_count < MP4_MAX_CHAPTERS; ++chunk) {
        uint32_t chunk_number = chunk + 1U;
        uint32_t per_chunk;
        uint64_t sample_pos = chapter_track->stco_chunk_offset[chunk];
        uint32_t j;
        while (stsc + 1U < chapter_track->stsc_entry_count &&
               chunk_number >= chapter_track->stsc_first_chunk[stsc + 1U])
            stsc++;
        per_chunk = chapter_track->stsc_samples_per_chunk[stsc];
        if (per_chunk == 0) break;
        for (j = 0; j < per_chunk && sample < chapter_track->stsz_entry_count &&
                    info->chapter_count < MP4_MAX_CHAPTERS; ++j, ++sample) {
            uint32_t size = mp4info_sample_size(chapter_track, sample);
            mp4info_chapter_t *chapter;
            while (stts < chapter_track->stts_entry_count && stts_left == 0) {
                stts++;
                if (stts < chapter_track->stts_entry_count)
                    stts_left = chapter_track->stts_sample_count[stts];
            }
            if (size == 0 || sample_pos > info->file_size ||
                size > info->file_size - sample_pos)
                return;
            if (size <= sizeof(sample_buf) &&
                io_set_position64(info->handle, (int64_t)sample_pos) == (int64_t)sample_pos &&
                io_read_data(info->handle, sample_buf, size) == size) {
                chapter = &info->chapters[info->chapter_count];
                memset(chapter, 0, sizeof(*chapter));
                chapter->time_ms = ticks * 1000ULL / chapter_track->time_scale;
                mp4info_decode_chapter_text(sample_buf, size,
                                            chapter->title, sizeof(chapter->title));
                if (chapter->title[0] == 0)
                    snprintf(chapter->title, sizeof(chapter->title),
                             "Chapter %lu", (unsigned long)info->chapter_count + 1UL);
                if (info->chapter_count == 0 ||
                    chapter->time_ms > info->chapters[info->chapter_count - 1U].time_ms)
                    info->chapter_count++;
            }
            sample_pos += size;
            if (stts < chapter_track->stts_entry_count) {
                ticks += chapter_track->stts_sample_duration[stts];
                if (stts_left > 0) stts_left--;
            }
        }
    }
}

static mp4info_t* mp4info_open_internal(const char* filename,
                                      int metadata_only) {
	if (!filename)
		return 0;

	mp4info_t* info = (mp4info_t*)malloc(sizeof(mp4info_t));
	if (!info)
		return 0;
	memset(info, 0, sizeof(mp4info_t));
	
	buffered_io_t io;	
	int32_t result = io_open64(filename, (void*)(&io));
	if ( result < 0 ) {
		free(info);
		return 0;
	}
	info->handle = &io;
	info->file_size = (uint64_t)io_get_length64(&io);
	parse_atoms(info);
	if (info->parse_error) {
		io_close(&io);
		info->handle = 0;
		mp4info_close(info);
		return 0;
	}
	
	int i;
	for (i = 0; i < info->total_tracks && i < MP4_MAX_TRACKS; i++) {
		mp4info_track_t* track = info->tracks[i];
		if (!track || track->stsc_entry_count == 0 ||
		    !track->stsc_first_chunk || !track->stsc_samples_per_chunk ||
		    !track->stsc_sample_desc_id)
			continue;

		if (track->stsc_first_chunk[track->stsc_entry_count - 1] <
		    track->stco_entry_count) {
			track->stsc_first_chunk[track->stsc_entry_count] =
				track->stco_entry_count;
			track->stsc_samples_per_chunk[track->stsc_entry_count] =
				track->stsc_samples_per_chunk[track->stsc_entry_count - 1];
			track->stsc_sample_desc_id[track->stsc_entry_count] =
				track->stsc_sample_desc_id[track->stsc_entry_count - 1];
			track->stsc_entry_count += 1;
		}
	}

	if (!metadata_only) {
		mp4info_extract_quicktime_chapters(info);
		mp4info_sort_chapters(info);
	}
	io_close(&io);
	info->handle = 0;
	return info;
}

mp4info_t* mp4info_open(const char* filename) {
	return mp4info_open_internal(filename, 0);
}

mp4info_t* mp4info_open_metadata(const char* filename) {
	return mp4info_open_internal(filename, 1);
}

void mp4info_close(mp4info_t* info) {
	if (!info)
		return;

	int32_t i;

	for (i = 0; i < info->total_tracks; i++) {
		if (info->tracks[i]) {
			
			if (info->tracks[i]->stts_sample_count)
				free(info->tracks[i]->stts_sample_count);
			if (info->tracks[i]->stts_sample_duration)
				free(info->tracks[i]->stts_sample_duration);
			if (info->tracks[i]->ctts_sample_count)
				free(info->tracks[i]->ctts_sample_count);
			if (info->tracks[i]->ctts_sample_offset)
				free(info->tracks[i]->ctts_sample_offset);
			if (info->tracks[i]->stss_sync_sample)
				free(info->tracks[i]->stss_sync_sample);
			if (info->tracks[i]->stsc_first_chunk)
				free(info->tracks[i]->stsc_first_chunk);
			if (info->tracks[i]->stsc_samples_per_chunk)
				free(info->tracks[i]->stsc_samples_per_chunk);
			if (info->tracks[i]->stsc_sample_desc_id)
				free(info->tracks[i]->stsc_sample_desc_id);
			if (info->tracks[i]->stsz_sample_size_table)
				free(info->tracks[i]->stsz_sample_size_table);
			if (info->tracks[i]->stco_chunk_offset)
				free(info->tracks[i]->stco_chunk_offset);
			
			if (info->tracks[i]->avc_sps)
				free(info->tracks[i]->avc_sps);
			if (info->tracks[i]->avc_pps)
				free(info->tracks[i]->avc_pps);
			free(info->tracks[i]);
		}
	}
	free(info);
}

mp4meta_t* mp4meta_open(const char* filename) {
	if (!filename)
		return 0;

	mp4meta_t* meta = (mp4meta_t*)malloc(sizeof(mp4meta_t));
	if (!meta)
		return 0;
	memset(meta, 0, sizeof(mp4meta_t));
	
	buffered_io_t io;	
	int32_t result = io_open64(filename, (void*)(&io));
	if ( result < 0 ) {
		free(meta);
		return 0;
	}
	meta->handle = &io;
	parse_metas(meta);
	io_close(&io);
	meta->handle = 0;
	
	return meta;
}

void mp4meta_close(mp4meta_t* meta) {
	
	if (meta) {
		if ( meta->title )
			free(meta->title);
		if ( meta->artist )
			free(meta->artist);
		if ( meta->album )
			free(meta->album);
		free(meta);
	}
}
