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
#include <pspkernel.h>
#include <stdio.h>
#include <limits.h>
#include "mp4_file.h"
#include "../common/ppa_utf8.h"
#include "../common/ppa_video_limits.h"

/* Counts remain bounded: wide offsets do not permit an unbounded index.
 * The allocator can fail sooner on base-RAM models; never truncate to fit. */
#define MP4_MAX_INDEX_BYTES (12U * 1024U * 1024U)
#define MP4_NO_FILE_OFFSET UINT64_MAX

#define MP4_FOURCC_MP4A 0x6D703461U
#define MP4_FOURCC_MP3  0x6D703320U

static int mp4_battery_audio_looks_cbr(const mp4info_track_t *track)
{
	uint64_t total = 0;
	unsigned int minimum = 0xffffffffU;
	unsigned int maximum = 0U;
	unsigned int count;
	unsigned int i;
	unsigned int average;

	if (track == 0)
		return 0;
	if (track->stsz_sample_size != 0U)
		return 1;
	if (track->stsz_sample_size_table == 0 || track->stsz_entry_count == 0U)
		return 0;
	count = track->stsz_entry_count < 64U ? track->stsz_entry_count : 64U;
	for (i = 0; i < count; ++i) {
		unsigned int size = track->stsz_sample_size_table[i];
		if (size < minimum) minimum = size;
		if (size > maximum) maximum = size;
		total += size;
	}
	average = (unsigned int)(total / count);
	if (average == 0U)
		return 0;
	/* AAC CBR frames are not byte-identical, so accept a bounded 40%%
	 * sample-size envelope while rejecting obvious VBR tracks. */
	return (uint64_t)(maximum - minimum) * 100ULL <=
	       (uint64_t)average * 40ULL;
}

static int mp4_audio_track_supported(const mp4info_t *info, const mp4info_track_t *track) {
    struct ppu_audio_format format;
    return ppu_audio_format_mp4(info, track, &format);
}

void mp4_file_safe_constructor(struct mp4_file_struct *p) {
	p->info = 0;
	p->video_track_id = -1;
	p->audio_tracks = 0;
	p->audio_up_sample = 0;
	p->audio_resample_num = 1;
	p->audio_resample_den = 1;
	p->audio_channels = 0;
	p->video_stts_entry_count = 0;
	p->video_stts_sample_count = 0;
	p->video_stts_sample_duration = 0;
	p->video_ctts_entry_count = 0;
	p->video_ctts_sample_count = 0;
	p->video_ctts_sample_offset = 0;
	
	p->subtitle_tracks = 0;
	int i;
	for(i = 0; i < 6; i++) {
		p->audio_time_scale[i] = 0;
        p->audio_stts_cursor[i] = 0;
        p->audio_stts_cursor_sample[i] = p->audio_stts_cursor_ticks[i] = 0;
		p->audio_sample_count[i] = 0;
		p->audio_stts_entry_count[i] = 0;
		p->audio_stts_sample_count[i] = 0;
		p->audio_stts_sample_duration[i] = 0;
	}
	for(i = 0; i < 4; i++) {
		p->subtitle_track_time[i] = 0;
		p->subtitle_track_time_count[i] = 0;
		p->subtitle_track_time_scale[i] = 0;
	}
	
	p->maximum_video_sample_size = 0;
	
	p->avc_sps_size = 0;
	p->avc_sps = 0;
	p->avc_pps_size = 0;
	p->avc_pps = 0;
	p->avc_nal_prefix_size = 0;
	
	p->sample_count = 0;
	p->samples = 0;
	p->index_count = 0;
	p->indexes = 0;
	
	p->is_not_interlace = 0;
	p->first_video_offset = MP4_NO_FILE_OFFSET;
	p->first_video_sample = 0;
	p->first_audio_offset = MP4_NO_FILE_OFFSET;
	p->first_audio_sample = 0;
	
	p->seek_duration = 5000;
	p->duration_ms = 0;
	p->chapter_count = 0;
	memset(p->chapters, 0, sizeof(p->chapters));
	
}

void mp4_file_close(struct mp4_file_struct *p) {
	if (p->video_stts_sample_count != 0)
		free_64(p->video_stts_sample_count);
	if (p->video_stts_sample_duration != 0)
		free_64(p->video_stts_sample_duration);
	if (p->video_ctts_sample_count != 0)
		free_64(p->video_ctts_sample_count);
	if (p->video_ctts_sample_offset != 0)
		free_64(p->video_ctts_sample_offset);
	p->video_stts_sample_count = 0;
	p->video_stts_sample_duration = 0;
	p->video_stts_entry_count = 0;
	p->video_ctts_sample_count = 0;
	p->video_ctts_sample_offset = 0;
	p->video_ctts_entry_count = 0;
	if (p->info != 0) {
		mp4info_close(p->info);
	}
	
	int i;
	for(i=0; i < 6; i++) {
		/* These arrays originate in atom_calloc_array, not malloc_64. */
		free(p->audio_stts_sample_count[i]);
		free(p->audio_stts_sample_duration[i]);
		p->audio_stts_sample_count[i] = 0;
		p->audio_stts_sample_duration[i] = 0;
	}
	for(i=0; i < 4; i++) {
		if ( p->subtitle_track_time[i] )
			free_64(p->subtitle_track_time[i]);
	}
	
	if ( p->avc_sps )
		free_64(p->avc_sps);
	if ( p->avc_pps )
		free_64(p->avc_pps);
	if ( p->samples )
		free_64(p->samples);
	if ( p->indexes )
		free_64(p->indexes);
	mp4_file_safe_constructor(p);
}

static int mp4_track_is_playback_indexed(const mp4info_track_t *track)
{
    return track != 0 && track->type != MP4_TRACK_SYSTEM &&
           track->stco_entry_count != 0;
}

static int mp4_track_list_contains(const int *ids, unsigned int count, int id)
{
    unsigned int i;
    for (i = 0; i < count; ++i) {
        if (ids[i] == id)
            return 1;
    }
    return 0;
}

static void mp4_track_list_add(int *ids, unsigned int *count,
                               unsigned int capacity, int id)
{
    if (ids == 0 || count == 0 || *count >= capacity || id < 0 ||
        mp4_track_list_contains(ids, *count, id))
        return;
    ids[(*count)++] = id;
}

struct mp4_chunk_heap_entry {
    uint64_t offset;
    int track_id;
};

static int mp4_chunk_heap_less(const struct mp4_chunk_heap_entry *a,
                               const struct mp4_chunk_heap_entry *b)
{
    if (a->offset != b->offset)
        return a->offset < b->offset;
    return a->track_id < b->track_id;
}

static void mp4_chunk_heap_push(struct mp4_chunk_heap_entry *heap,
                                unsigned int *count,
                                unsigned int capacity,
                                struct mp4_chunk_heap_entry value)
{
    unsigned int index;
    if (heap == 0 || count == 0 || *count >= capacity ||
        value.offset == MP4_NO_FILE_OFFSET)
        return;
    index = (*count)++;
    while (index != 0U) {
        unsigned int parent = (index - 1U) >> 1;
        if (!mp4_chunk_heap_less(&value, &heap[parent]))
            break;
        heap[index] = heap[parent];
        index = parent;
    }
    heap[index] = value;
}

static struct mp4_chunk_heap_entry mp4_chunk_heap_pop(
    struct mp4_chunk_heap_entry *heap, unsigned int *count)
{
    struct mp4_chunk_heap_entry result = { MP4_NO_FILE_OFFSET, -1 };
    struct mp4_chunk_heap_entry tail;
    unsigned int index = 0U;
    if (heap == 0 || count == 0 || *count == 0U)
        return result;
    result = heap[0];
    (*count)--;
    if (*count == 0U)
        return result;
    tail = heap[*count];
    while (1) {
        unsigned int left = index * 2U + 1U;
        unsigned int right = left + 1U;
        unsigned int child;
        if (left >= *count)
            break;
        child = left;
        if (right < *count && mp4_chunk_heap_less(&heap[right], &heap[left]))
            child = right;
        if (!mp4_chunk_heap_less(&heap[child], &tail))
            break;
        heap[index] = heap[child];
        index = child;
    }
    heap[index] = tail;
    return result;
}

static uint64_t mp4_get_trunk_offset(mp4info_track_t* track, unsigned int trunk) {
	if ( trunk >= track->stco_entry_count )
		return MP4_NO_FILE_OFFSET;
	else
		return track->stco_chunk_offset[trunk];
}

unsigned int mp4_get_sample_size(mp4info_track_t* track, unsigned int sample) {
	if (track == 0 || sample >= track->stsz_entry_count)
		return UINT_MAX;
	if (track->stsz_sample_size != 0)
		return track->stsz_sample_size;
	if (track->stsz_sample_size_table == 0)
		return UINT_MAX;
	return track->stsz_sample_size_table[sample];
}

unsigned int mp4_get_sample_count(mp4info_track_t* track) {
	unsigned int i;
	uint64_t sample_count = 0;

	if (track == 0 || track->stts_entry_count == 0 ||
	    track->stts_sample_count == 0)
		return 0;
	for (i = 0; i < track->stts_entry_count; i++) {
		sample_count += track->stts_sample_count[i];
		if (sample_count > UINT_MAX)
			return UINT_MAX;
	}
	return (unsigned int)sample_count;
}

static uint64_t mp4_stts_timestamp_ticks(uint32_t entry_count,
                                         const uint32_t *counts,
                                         const uint32_t *durations,
                                         uint32_t sample_index)
{
	uint64_t ticks = 0;
	unsigned int i;

	for (i = 0; i < entry_count; ++i) {
		uint32_t count = counts[i];
		if (sample_index < count) {
			ticks += (uint64_t)sample_index * durations[i];
			return ticks;
		}
		ticks += (uint64_t)count * durations[i];
		sample_index -= count;
	}
	return ticks;
}

static int64_t mp4_ctts_offset_ticks(const struct mp4_file_struct *p,
                                     unsigned int sample_index)
{
	unsigned int i;
	if (p == 0)
		return 0;
	for (i = 0; i < p->video_ctts_entry_count; ++i) {
		if (sample_index < p->video_ctts_sample_count[i])
			return p->video_ctts_sample_offset[i];
		sample_index -= p->video_ctts_sample_count[i];
	}
	return 0;
}

static unsigned int mp4_ticks_to_ms(uint64_t ticks, unsigned int scale)
{
	uint64_t seconds, milliseconds;
	if (scale == 0) return 0;
	seconds = ticks / scale;
	if (seconds > (uint64_t)INT_MAX / 1000ULL) return INT_MAX;
	milliseconds = seconds * 1000ULL + (ticks % scale) * 1000ULL / scale;
	return milliseconds > INT_MAX ? INT_MAX : (unsigned int)milliseconds;
}

/* Decode-owner-only cursor over immutable, validated STTS runs. Sequential
 * packets visit each run once instead of rescanning the whole table. A backward
 * seek restarts the cursor; each selectable track retains its own position. */
static int mp4_audio_timing(struct mp4_file_struct *p, unsigned int slot,
                            unsigned int sample, uint64_t *position, uint32_t *duration)
{
    uint32_t entry;
    uint64_t first, ticks;
    if (!p || slot >= (unsigned int)p->audio_tracks || slot >= 6 ||
        sample >= p->audio_sample_count[slot] ||
        !p->audio_stts_sample_count[slot] || !p->audio_stts_sample_duration[slot]) return 0;
    entry = p->audio_stts_cursor[slot];
    first = p->audio_stts_cursor_sample[slot];
    ticks = p->audio_stts_cursor_ticks[slot];
    if (sample < first || entry >= p->audio_stts_entry_count[slot])
        entry = 0, first = 0, ticks = 0;
    while (entry < p->audio_stts_entry_count[slot]) {
        uint32_t count = p->audio_stts_sample_count[slot][entry];
        uint32_t delta = p->audio_stts_sample_duration[slot][entry];
        if ((uint64_t)sample - first < count) {
            p->audio_stts_cursor[slot] = entry;
            p->audio_stts_cursor_sample[slot] = first;
            p->audio_stts_cursor_ticks[slot] = ticks;
            *position = ticks + ((uint64_t)sample - first) * delta;
            *duration = delta;
            return 1;
        }
        first += count; ticks += (uint64_t)count * delta; ++entry;
    }
    return 0;
}

uint64_t mp4_file_audio_sample_position(struct mp4_file_struct *p,
                                       unsigned int slot, unsigned int sample)
{
    uint64_t ticks = 0;
    uint32_t duration;
    (void)mp4_audio_timing(p, slot, sample, &ticks, &duration);
    return ticks;
}
uint32_t mp4_file_audio_sample_duration(struct mp4_file_struct *p,
                                       unsigned int slot, unsigned int sample)
{
    uint64_t ticks;
    uint32_t duration = 0;
    (void)mp4_audio_timing(p, slot, sample, &ticks, &duration);
    return duration;
}

unsigned int mp4_file_audio_timestamp_ms(struct mp4_file_struct *p,
                                         unsigned int audio_slot,
                                         unsigned int sample_index)
{
    uint64_t ticks;
    uint32_t duration;
    if (!mp4_audio_timing(p, audio_slot, sample_index, &ticks, &duration)) return 0;
    return mp4_ticks_to_ms(ticks, p->audio_time_scale[audio_slot]);
}

unsigned int mp4_file_audio_sample_at_ms(const struct mp4_file_struct *p,
                                        unsigned int audio_slot,
                                        unsigned int timestamp)
{
	uint64_t ticks;
	unsigned int sample = 0, i;
	if (!p || audio_slot >= (unsigned int)p->audio_tracks || audio_slot >= 6U ||
	    !p->audio_sample_count[audio_slot] ||
	    !p->audio_stts_sample_count[audio_slot] ||
	    !p->audio_stts_sample_duration[audio_slot]) return 0;
	ticks = (uint64_t)timestamp * p->audio_time_scale[audio_slot] / 1000ULL;
	for (i = 0; i < p->audio_stts_entry_count[audio_slot]; ++i) {
		unsigned int count = p->audio_stts_sample_count[audio_slot][i];
		unsigned int duration = p->audio_stts_sample_duration[audio_slot][i];
		uint64_t run = (uint64_t)count * duration;
		if (duration != 0 && ticks < run) {
			sample += (unsigned int)(ticks / duration);
			return sample;
		}
		ticks -= run;
		sample += count;
	}
	return p->audio_sample_count[audio_slot] - 1U;
}

unsigned int mp4_file_video_timestamp_ms(const struct mp4_file_struct *p,
                                         unsigned int sample_index)
{
	uint64_t dts_ticks;
	int64_t pts_ticks;

	if (p == 0 || p->video_rate == 0 || p->video_stts_entry_count == 0 ||
	    p->video_stts_sample_count == 0 ||
	    p->video_stts_sample_duration == 0)
		return 0;

	dts_ticks = mp4_stts_timestamp_ticks(p->video_stts_entry_count,
	                                     p->video_stts_sample_count,
	                                     p->video_stts_sample_duration,
	                                     sample_index);
	pts_ticks = (int64_t)dts_ticks + mp4_ctts_offset_ticks(p, sample_index);
	if (pts_ticks < 0)
		pts_ticks = 0;
	return mp4_ticks_to_ms((uint64_t)pts_ticks, p->video_rate);
}

static int mp4_is_keyframe(const mp4info_track_t *track,
                           unsigned int sample,
                           unsigned int implicit_sync_stride,
                           unsigned int *stss_cursor)
{
	/* ISO BMFF defines every sample as a sync sample when stss is absent.
	 * Build a sparse five-second seek index in that case: indexing every
	 * sample wastes several megabytes on long videos, while indexing only
	 * sample zero makes seeking needlessly decode from the beginning. */
	if (track->stss_entry_count == 0) {
		if (implicit_sync_stride == 0)
			implicit_sync_stride = 1;
		return (sample % implicit_sync_stride) == 0;
	}
    if (stss_cursor == 0)
        return 0;
    while (*stss_cursor < track->stss_entry_count &&
           track->stss_sync_sample[*stss_cursor] < sample + 1U)
        (*stss_cursor)++;
    if (*stss_cursor < track->stss_entry_count &&
        track->stss_sync_sample[*stss_cursor] == sample + 1U) {
        (*stss_cursor)++;
        return 1;
    }
	return 0;
}

char *mp4_file_build_index(struct mp4_file_struct *p) {
	int i;
	unsigned int j;
	unsigned int ui;
	unsigned int trunk[MP4_MAX_TRACKS];
	unsigned int stsc_index[MP4_MAX_TRACKS];
	unsigned int next_first_sample[MP4_MAX_TRACKS];
	unsigned int current_sample = 0;
	unsigned int current_index = 0;
	unsigned int implicit_sync_stride = 0;
	unsigned int total_chunks = 0;
	unsigned int processed_chunks = 0;
	unsigned int stss_cursor = 0;
	int indexed_track_ids[11];
	unsigned int indexed_track_count = 0U;
	unsigned int indexed_subtitle_count = 0U;
	struct mp4_chunk_heap_entry chunk_heap[11];
	unsigned int chunk_heap_count = 0U;

	char *result = 0;

	mp4info_track_t *v_track;
	mp4info_track_t *a_track;

	v_track = p->info->tracks[p->video_track_id];
	a_track = p->info->tracks[p->audio_track_ids[0]];

	if (v_track == 0 || a_track == 0 ||
	    v_track->stco_entry_count == 0 || a_track->stco_entry_count == 0 ||
	    v_track->stco_chunk_offset == 0 || a_track->stco_chunk_offset == 0) {
		mp4_file_close(p);
		return "mp4_file_open: missing media chunk table";
	}

	/* Build only the tracks consumed by playback: selected AVC, supported AAC
	 * tracks, and the first four subtitle tracks exposed by the UI. Files with
	 * dozens of language tracks previously multiplied every chunk-merge pass. */
	mp4_track_list_add(indexed_track_ids, &indexed_track_count, 11U,
	                   p->video_track_id);
	for (i = 0; i < p->audio_tracks && i < 6; ++i)
		mp4_track_list_add(indexed_track_ids, &indexed_track_count, 11U,
		                   p->audio_track_ids[i]);
	for (i = 0; i < p->info->total_tracks && indexed_track_count < 11U; ++i) {
		mp4info_track_t *track = p->info->tracks[i];
		if (indexed_subtitle_count >= 4U)
			break;
		if (track != 0 && track->type == MP4_TRACK_SUBTITLE) {
			mp4_track_list_add(indexed_track_ids, &indexed_track_count, 11U, i);
			indexed_subtitle_count++;
		}
	}

	if ((v_track->stco_chunk_offset[v_track->stco_entry_count - 1] <
	     a_track->stco_chunk_offset[0]) ||
	    (v_track->stco_chunk_offset[0] >
	     a_track->stco_chunk_offset[a_track->stco_entry_count - 1])) {
		int unsupported_extra_track = 0;
		for (j = 0; j < indexed_track_count; ++j) {
			int track_id = indexed_track_ids[j];
			mp4info_track_t *candidate = p->info->tracks[track_id];
			if (!mp4_track_is_playback_indexed(candidate) ||
			    track_id == p->video_track_id ||
			    track_id == p->audio_track_ids[0])
				continue;
			unsupported_extra_track = 1;
			break;
		}
		if (unsupported_extra_track) {
			mp4_file_close(p);
			return("mp4_file_open: unsupported separated auxiliary media track");
		}
		p->is_not_interlace = 1;
	}

	for (j = 0; j < indexed_track_count; j++) {
		int track_id = indexed_track_ids[j];
		mp4info_track_t *track = p->info->tracks[track_id];
		unsigned int track_samples;
		uint64_t total;

		if (!mp4_track_is_playback_indexed(track))
			continue;
		track_samples = mp4_get_sample_count(track);
		/* sample_index packs an 8-bit track slot and 24-bit local sample.
		 * Large files must not silently wrap that independent identity format. */
		if (track_id > 255 || track_samples > 0x01000000U || track_samples == 0 ||
		    track->stsz_entry_count != track_samples) {
			mp4_file_close(p);
			return "mp4_file_open: inconsistent sample tables";
		}
		/* Public playback timestamps are signed milliseconds. Reject a clock
		 * range that cannot be represented before indexes/packets can wrap. */
		if (track->time_scale == 0 || mp4_stts_timestamp_ticks(
		        track->stts_entry_count, track->stts_sample_count,
		        track->stts_sample_duration, track_samples) >
		    (uint64_t)INT_MAX * track->time_scale / 1000ULL) {
			mp4_file_close(p);
			return "mp4_file_open: unsupported track timestamp range";
		}
		total = (uint64_t)p->sample_count + track_samples;
		if (total > UINT_MAX) {
			mp4_file_close(p);
			return "mp4_file_open: too many samples";
		}
		p->sample_count = (unsigned int)total;

		total = (uint64_t)total_chunks + track->stco_entry_count;
		if (total > UINT_MAX) {
			mp4_file_close(p);
			return "mp4_file_open: too many chunks";
		}
		total_chunks = (unsigned int)total;
	}
	if (p->sample_count == 0) {
		mp4_file_close(p);
		return "mp4_file_open: no media samples";
	}

	p->index_count = v_track->stss_entry_count;
	if (p->index_count == 0) {
		/* Approximately one entry per five seconds, derived from the exact
		 * track duration where possible and capped to a valid frame stride. */
		if (p->duration_ms != 0 && p->number_of_video_frames != 0) {
			uint64_t frames_per_window =
				((uint64_t)p->number_of_video_frames * 5000ULL +
				 (uint64_t)p->duration_ms - 1ULL) /
				(uint64_t)p->duration_ms;
			if (frames_per_window == 0)
				frames_per_window = 1;
			if (frames_per_window > 0xffffffffULL)
				frames_per_window = 0xffffffffULL;
			implicit_sync_stride = (unsigned int)frames_per_window;
		}
		else {
			implicit_sync_stride = 150U;
		}

		p->index_count = (unsigned int)(
			((uint64_t)p->number_of_video_frames +
			 (uint64_t)implicit_sync_stride - 1ULL) /
			(uint64_t)implicit_sync_stride);
		if (p->index_count == 0)
			p->index_count = 1;
	}

	{
		uint64_t sample_bytes = (uint64_t)p->sample_count * sizeof(*p->samples);
		uint64_t index_bytes = (uint64_t)p->index_count * sizeof(*p->indexes);
		if (sample_bytes + index_bytes > MP4_MAX_INDEX_BYTES) {
			mp4_file_close(p);
			return "mp4_file_open: sample/seek index exceeds PSP memory budget";
		}
		/* The combined bound also protects malloc_64's signed-int argument. */
		p->samples = malloc_64((int)sample_bytes);
	}
	if (!p->samples) {
		mp4_file_close(p);
		return("mp4_file_open: can't malloc samples buffer");
	}

	p->indexes = malloc_64((int)((uint64_t)p->index_count * sizeof(*p->indexes)));
	if (!p->indexes) {
		mp4_file_close(p);
		return("mp4_file_open: can't malloc indexes buffer");
	}

	for (i = 0; i < MP4_MAX_TRACKS; i++) {
		trunk[i] = 0;
		stsc_index[i] = 0;
		next_first_sample[i] = 0;
	}

	for (j = 0; j < indexed_track_count; ++j) {
		int track_id = indexed_track_ids[j];
		struct mp4_chunk_heap_entry entry;
		entry.track_id = track_id;
		entry.offset = mp4_get_trunk_offset(p->info->tracks[track_id], 0U);
		mp4_chunk_heap_push(chunk_heap, &chunk_heap_count, 11U, entry);
	}

	while (chunk_heap_count != 0U) {
		struct mp4_chunk_heap_entry heap_entry =
			mp4_chunk_heap_pop(chunk_heap, &chunk_heap_count);
		int current_track = heap_entry.track_id;
		uint64_t min_offset = heap_entry.offset;
		unsigned int first_sample = 0;
		unsigned int last_sample = 0;
		unsigned int samples_per_chunk = 0;
		unsigned int chunk_number = 0;

		{
			mp4info_track_t *track = p->info->tracks[current_track];

			if (track->stsc_entry_count == 0) {
				mp4_file_close(p);
				return("mp4_file_open: empty stsc table");
			}

			chunk_number = trunk[current_track] + 1;

			while ((stsc_index[current_track] + 1) < track->stsc_entry_count &&
			       chunk_number >= track->stsc_first_chunk[stsc_index[current_track] + 1]) {
				stsc_index[current_track]++;
			}

			samples_per_chunk =
				track->stsc_samples_per_chunk[stsc_index[current_track]];

			if (samples_per_chunk == 0) {
				mp4_file_close(p);
				return("mp4_file_open: zero samples per chunk");
			}

			first_sample = next_first_sample[current_track];
			if (first_sample >= track->stsz_entry_count ||
			    samples_per_chunk > track->stsz_entry_count - first_sample) {
				mp4_file_close(p);
				return "mp4_file_open: chunk exceeds sample table";
			}
			last_sample = first_sample + samples_per_chunk - 1;

			for (ui = first_sample; ui <= last_sample; ui++) {
				if (current_sample >= p->sample_count) {
					mp4_file_close(p);
					return("mp4_file_open: sample index overflow");
				}

				p->samples[current_sample].sample_index =
					((uint32_t)current_track << 24) | (ui & 0x00FFFFFFU);
				p->samples[current_sample].file_offset = min_offset;

				p->samples[current_sample].sample_size =
					mp4_get_sample_size(track, ui);
				if (p->samples[current_sample].sample_size == UINT_MAX ||
				    p->samples[current_sample].sample_size == 0U ||
				    min_offset > p->info->file_size ||
				    p->samples[current_sample].sample_size >
				        p->info->file_size - min_offset) {
					mp4_file_close(p);
					return "mp4_file_open: invalid or out-of-file sample table";
				}

				if (current_track == p->video_track_id) {
					if (MP4_NO_FILE_OFFSET == p->first_video_offset) {
						p->first_video_offset = min_offset;
						p->first_video_sample = current_sample;
					}

					if (p->samples[current_sample].sample_size >
						p->maximum_video_sample_size) {
						p->maximum_video_sample_size =
							p->samples[current_sample].sample_size;
					}

					if (mp4_is_keyframe(track, ui, implicit_sync_stride,
					                    &stss_cursor)) {
						if (current_index >= p->index_count) {
							mp4_file_close(p);
							return("mp4_file_open: keyframe index overflow");
						}

						{
							uint64_t timestamp;
							uint64_t ticks = mp4_stts_timestamp_ticks(
								track->stts_entry_count,
								track->stts_sample_count,
								track->stts_sample_duration, ui);
							timestamp = mp4_ticks_to_ms(ticks, p->video_rate);

							p->indexes[current_index].timestamp =
								timestamp;
							p->indexes[current_index].sample_index =
								current_sample;
							p->indexes[current_index].offset =
								min_offset;

							current_index++;
						}
					}
				}
				else if (current_track == p->audio_track_ids[0]) {
					if (MP4_NO_FILE_OFFSET == p->first_audio_offset) {
						p->first_audio_offset = min_offset;
						p->first_audio_sample = current_sample;
					}
				}

				min_offset += p->samples[current_sample].sample_size;
				current_sample++;
			}

			next_first_sample[current_track] += samples_per_chunk;
			trunk[current_track] += 1;
			processed_chunks++;
			{
				struct mp4_chunk_heap_entry next_entry;
				next_entry.track_id = current_track;
				next_entry.offset = mp4_get_trunk_offset(
					track, trunk[current_track]);
				mp4_chunk_heap_push(chunk_heap, &chunk_heap_count, 11U,
				                    next_entry);
			}
		}
	}

	/* The merged table is only safe when every declared chunk and sample was
	 * consumed exactly once. A short merge leaves zero-initialized records
	 * that later look like valid media offsets; an overlong merge is caught
	 * above. Validate per-track consumption as well so inconsistent stsc/stco
	 * tables fail during admission rather than during asynchronous playback. */
	if (processed_chunks != total_chunks || current_sample != p->sample_count) {
		mp4_file_close(p);
		return "mp4_file_open: incomplete media index";
	}
	for (j = 0; j < indexed_track_count; ++j) {
		int track_id = indexed_track_ids[j];
		mp4info_track_t *track = p->info->tracks[track_id];
		if (!mp4_track_is_playback_indexed(track))
			continue;
		if (trunk[track_id] != track->stco_entry_count ||
		    next_first_sample[track_id] != track->stsz_entry_count) {
			mp4_file_close(p);
			return "mp4_file_open: inconsistent chunk mapping";
		}
	}

	/* Malformed stss tables can reference samples beyond the media track.
	 * Never expose uninitialized index entries to the binary-search seeker. */
	if (current_index == 0) {
		mp4_file_close(p);
		return "mp4_file_open: video track has no usable sync samples";
	}
	p->index_count = current_index;

	if (!p->is_not_interlace) {
		for (i = 0; i < p->info->total_tracks; i++) {
			unsigned int sub_slot;
			mp4info_track_t *track = p->info->tracks[i];

			if (track->type != MP4_TRACK_SUBTITLE)
				continue;

			p->subtitle_tracks++;
			sub_slot = p->subtitle_tracks - 1;

			p->subtitle_track_ids[sub_slot] = i;
			p->subtitle_track_time_count[sub_slot] =
				track->stts_entry_count;
			p->subtitle_track_time_scale[sub_slot] =
				track->time_scale ? track->time_scale : 1000;

			p->subtitle_track_time[sub_slot] =
				malloc_64(track->stts_entry_count *
				          2 *
				          sizeof(unsigned int));

			if (!p->subtitle_track_time[sub_slot]) {
				p->subtitle_track_time_scale[sub_slot] = 0;
				p->subtitle_track_time_count[sub_slot] = 0;
				p->subtitle_tracks--;
				break;
			}
			else {
				for (j = 0; j < track->stts_entry_count; j++) {
					p->subtitle_track_time[sub_slot][2 * j] =
						track->stts_sample_count[j];
					p->subtitle_track_time[sub_slot][2 * j + 1] =
						track->stts_sample_duration[j];
				}
			}

			if (p->subtitle_tracks == 4)
				break;
		}
	}

	/* Retain compact decode-timestamp runs after mp4info is closed. This keeps
	 * VFR timing exact without allocating one timestamp per video sample. */
	p->video_stts_entry_count = v_track->stts_entry_count;
	if (p->video_stts_entry_count != 0) {
		unsigned int bytes = p->video_stts_entry_count * sizeof(uint32_t);
		p->video_stts_sample_count = (uint32_t *)malloc_64(bytes);
		p->video_stts_sample_duration = (uint32_t *)malloc_64(bytes);
		if (p->video_stts_sample_count == 0 ||
		    p->video_stts_sample_duration == 0) {
			mp4_file_close(p);
			return "mp4_file_open: can't retain VFR timing table";
		}
		memcpy(p->video_stts_sample_count, v_track->stts_sample_count, bytes);
		memcpy(p->video_stts_sample_duration, v_track->stts_sample_duration, bytes);
	}

	p->video_ctts_entry_count = v_track->ctts_entry_count;
	if (p->video_ctts_entry_count != 0) {
		unsigned int bytes = p->video_ctts_entry_count * sizeof(uint32_t);
		p->video_ctts_sample_count = (uint32_t *)malloc_64(bytes);
		p->video_ctts_sample_offset = (int32_t *)malloc_64(bytes);
		if (p->video_ctts_sample_count == 0 ||
		    p->video_ctts_sample_offset == 0) {
			mp4_file_close(p);
			return "mp4_file_open: can't retain composition timing table";
		}
		memcpy(p->video_ctts_sample_count, v_track->ctts_sample_count, bytes);
		memcpy(p->video_ctts_sample_offset, v_track->ctts_sample_offset, bytes);
	}

	/* Retain audio STTS directly instead of inventing packet timestamps from
	 * codec frame size. Container timescales need not equal the sample rate.
	 * Moving the already bounded arrays avoids another allocation/copy peak. */
	for (i = 0; i < p->audio_tracks; ++i) {
		mp4info_track_t *audio = p->info->tracks[p->audio_track_ids[i]];
		p->audio_time_scale[i] = audio->time_scale;
		p->audio_sample_count[i] = audio->stsz_entry_count;
		p->audio_stts_entry_count[i] = audio->stts_entry_count;
		p->audio_stts_sample_count[i] = audio->stts_sample_count;
		p->audio_stts_sample_duration[i] = audio->stts_sample_duration;
		audio->stts_sample_count = 0;
		audio->stts_sample_duration = 0;
		audio->stts_entry_count = 0;
	}
	mp4info_close(p->info);
	p->info = 0;

	return(result);
}

char *mp4_file_open(struct mp4_file_struct *p, char *s) {
	char *result;
	int i;
	unsigned int ui;
	int saw_video_track = 0;
	int saw_oversize_avc = 0;
	int saw_unsupported_video_codec = 0;
	int saw_missing_avcc = 0;

	mp4_file_safe_constructor(p);

	p->info = mp4info_open(s);

	if (p->info != 0) {
		uint32_t ci;
		p->chapter_count = p->info->chapter_count > PPA_MAX_CHAPTERS ?
			PPA_MAX_CHAPTERS : (unsigned int)p->info->chapter_count;
		for (ci = 0; ci < p->chapter_count; ++ci) {
			uint64_t ms = p->info->chapters[ci].time_ms;
			p->chapters[ci].time_ms = ms > 0xffffffffULL ? 0xffffffffU : (uint32_t)ms;
			ppa_utf8_sanitize_copy(p->chapters[ci].title,
				sizeof(p->chapters[ci].title), p->info->chapters[ci].title);
		}
	}

	if (p->info == 0) {
		mp4_file_close(p);
		return("mp4_file_open: can't open file");
	}

	for (i = 0; i < p->info->total_tracks; i++) {
		mp4info_track_t *track = p->info->tracks[i];

		if (track->type != MP4_TRACK_VIDEO)
			continue;

		saw_video_track = 1;
		if (track->width < 1 || track->height < 1) {
			continue;
		}

		if (track->video_type != 0x61766331U /* avc1 */) {
			saw_unsupported_video_codec = 1;
			continue;
		}

		/* The hardware decoder and conversion surfaces share the same coded
		 * frame contract.  Display scaling is performed by the GE at presentation;
		 * no larger software-decoded intermediate is admitted. */
		if (!PPA_VIDEO_DIMENSIONS_VALID(track->width, track->height)) {
			saw_oversize_avc = 1;
			continue;
		}

		/* Baseline streams above 480x272 are valid in the large mode-5 context.
		 * The old screen-resolution rejection contradicted the decoder's existing
		 * 720x480 path and the battery-saver admission contract. */
		if (track->avc_sps == 0 || track->avc_sps_size == 0 ||
		    track->avc_pps == 0 || track->avc_pps_size == 0) {
			saw_missing_avcc = 1;
			continue;
		}

		p->video_track_id = i;
		p->video_type = track->video_type;
		p->avc_profile = track->avc_profile;
		p->avc_sps_size = track->avc_sps_size;
		p->avc_pps_size = track->avc_pps_size;
		p->avc_nal_prefix_size = track->avc_nal_prefix_size;

		p->avc_sps = malloc_64(p->avc_sps_size);
		if (!p->avc_sps) {
			mp4_file_close(p);
			return("mp4_file_open: can't malloc avc_sps buffer");
		}
		memcpy(p->avc_sps, track->avc_sps, p->avc_sps_size);

		p->avc_pps = malloc_64(p->avc_pps_size);
		if (!p->avc_pps) {
			mp4_file_close(p);
			return("mp4_file_open: can't malloc avc_pps buffer");
		}
		memcpy(p->avc_pps, track->avc_pps, p->avc_pps_size);

		break;
	}

	if (p->video_track_id < 0) {
		char *video_error =
			saw_oversize_avc ?
			"mp4_file_open: AVC coded resolution exceeds the PSP 720x480 playback limit" :
			saw_missing_avcc ?
			"mp4_file_open: AVC track is missing usable avcC SPS/PPS" :
			saw_unsupported_video_codec ?
			"mp4_file_open: MP4 video codec is not AVC (avc1)" :
			saw_video_track ?
			"mp4_file_open: no usable AVC video track" :
			"mp4_file_open: MP4 contains no video track";
		mp4_file_close(p);
		return video_error;
	}

	for (i = 0; i < p->info->total_tracks; i++) {
		mp4info_track_t *track = p->info->tracks[i];

		if (!mp4_audio_track_supported(p->info, track))
			continue;

		if (p->audio_tracks == 0) {
			p->audio_tracks++;
			p->audio_track_ids[p->audio_tracks - 1] = i;
			p->audio_type = track->audio_type;
			p->audio_channels = track->channels;

		}
		else {
			mp4info_track_t *old_track =
				p->info->tracks[p->audio_track_ids[p->audio_tracks - 1]];

			if (old_track->audio_type != track->audio_type)
				continue;

			if (old_track->samplerate != track->samplerate)
				continue;
			if (old_track->channels != track->channels)
				continue;

			p->audio_tracks++;
			p->audio_track_ids[p->audio_tracks - 1] = i;
		}

		if (!ppu_audio_format_mp4(p->info, track, &p->audio_formats[p->audio_tracks - 1])) {
            mp4_file_close(p); return "audio: invalid track configuration";
        }
        p->audio_battery_cbr[p->audio_tracks - 1] = mp4_battery_audio_looks_cbr(track);
		if (p->audio_tracks == 6)
			break;
	}

	if (p->audio_tracks == 0) {
		mp4_file_close(p);
		return("mp4_file_open: can't found audio track in mp4 file");
	}

	p->video_width = p->info->tracks[p->video_track_id]->width;
	p->video_height = p->info->tracks[p->video_track_id]->height;

	p->number_of_video_frames = 0;

	for (ui = 0;
	     ui < p->info->tracks[p->video_track_id]->stts_entry_count;
	     ++ui) {
		p->number_of_video_frames +=
			p->info->tracks[p->video_track_id]->stts_sample_count[ui];
	}

	if (p->number_of_video_frames == 0) {
		mp4_file_close(p);
		return "mp4_file_open: video track has no timing samples";
	}
	p->video_rate = p->info->tracks[p->video_track_id]->time_scale;
	p->video_scale =
		p->info->tracks[p->video_track_id]->duration /
		p->number_of_video_frames;
	if (p->video_rate == 0 || p->video_scale == 0) {
		mp4_file_close(p);
		return "mp4_file_open: invalid video timescale";
	}
	if ((uint64_t)p->video_rate > (uint64_t)p->video_scale * 61ULL) {
		mp4_file_close(p);
		return "mp4_file_open: frame rate above experimental 60 FPS limit";
	}

	if (p->video_rate != 0) {
		uint64_t duration_ms =
			((uint64_t)p->info->tracks[p->video_track_id]->duration * 1000ULL) /
			(uint64_t)p->video_rate;
		if (duration_ms > 0xffffffffULL)
			duration_ms = 0xffffffffULL;
		p->duration_ms = (unsigned int)duration_ms;
	}

    p->audio_actual_rate = p->audio_formats[0].rate;
    p->audio_rate = PPU_AUDIO_RATE;
    p->audio_scale = p->audio_formats[0].max_frames;
    p->audio_resample_scale = PPU_AUDIO_BLOCK;
    p->audio_resample_num = PPU_AUDIO_RATE;
    p->audio_resample_den = p->audio_actual_rate;
    /* Legacy producer scheduling hint: one 23-ms block per primary pass;
     * an optional second pass may refill after video without doing I/O busywork. */
    p->audio_up_sample = 0;
    p->audio_stereo = 1;

	result = mp4_file_build_index(p);

	if (result != 0) {
		return(result);
	}

	return(0);
}
