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
#include <stdio.h>
#include "mp4_read.h"
#include "common/ppa_playback_session.h"
#include "subtitle_parse.h"
#include "../common/ppa_packet_pool.h"
#include "../common/ppa_time.h"
#include "../common/ppa_hardware_profile.h"

#include <limits.h>
#include <stdint.h>

void mp4_read_release_output(struct mp4_read_output_struct *p);

typedef char* (*f_mp4_read_fill_buffer)(struct mp4_read_struct *, int );
static f_mp4_read_fill_buffer p_mp4_read_fill_buffer;

static int mp4_read_sample_exact(buffered_reader_t *reader,
                                 uint64_t file_offset,
                                 void *buffer,
                                 uint32_t size)
{
    unsigned int attempt;
    uint64_t offset = file_offset;

    if (reader == 0 || buffer == 0 || size == 0U)
        return 0;
    for (attempt = 0; attempt < 2U; ++attempt) {
        if (buffered_reader_position64(reader) != (int64_t)offset &&
            buffered_reader_seek64(reader, offset) < 0)
            continue;
        if (buffered_reader_read_exact(reader, buffer, size)) {
            
            return 1;
        }
        (void)buffered_reader_seek64(reader, offset);
    }
    return 0;
}

static struct ppa_packet_pool g_mp4_video_packet_pool;
static struct ppa_packet_pool g_mp4_audio_packet_pool;

static void clear_mp4_read_queue(struct mp4_read_output_struct *queue,
                                 unsigned int *queue_size,
                                 unsigned int *queue_front,
                                 unsigned int *queue_rear,
                                 unsigned int queue_max);

#define MP4_IO_STREAM_INTERLEAVED 0U
#define MP4_IO_STREAM_VIDEO       1U
#define MP4_IO_STREAM_AUDIO       2U
#define MP4_VIDEO_TRIM_FLOOR      (64U * 1024U)
#define MP4_AUDIO_TRIM_FLOOR      (16U * 1024U)

static uint32_t mp4_read_u64_to_u32(uint64_t value)
{
	return value > UINT_MAX ? UINT_MAX : (uint32_t)value;
}

static uint32_t mp4_read_nominal_ms(uint32_t scale, uint32_t rate)
{
	uint64_t value;

	if (scale == 0U || rate == 0U)
		return 1U;
	value = ppa_div_round_u64(1000ULL * (uint64_t)scale, rate);
	if (value == 0U)
		value = 1U;
	if (value > 1000U)
		value = 1000U;
	return (uint32_t)value;
}

static uint32_t mp4_read_byte_ceiling(uint32_t base, uint32_t maximum_packet)
{
	uint64_t minimum = (uint64_t)maximum_packet * 2ULL;

	if (minimum < base)
		minimum = base;
	return mp4_read_u64_to_u32(minimum);
}

static void mp4_read_schedule_readers(struct mp4_read_struct *p)
{
	const struct PpaQueueWatermark *selected;
	uint32_t video_margin;
	uint32_t audio_margin;

	if (p == 0)
		return;
	if (p->file.is_not_interlace) {
		ppa_queue_watermark_schedule_reader(&p->video_watermark, p->v_reader);
		if (p->file.audio_tracks > 0)
			ppa_queue_watermark_schedule_reader(&p->audio_watermark,
			                                    p->a_reader);
		return;
	}
	if (p->reader == 0)
		return;

	selected = ppa_session_audio_only() ? &p->audio_watermark : &p->video_watermark;
	if (!ppa_session_audio_only() && p->file.audio_tracks > 0) {
		video_margin = ppa_queue_watermark_margin_ms(&p->video_watermark);
		audio_margin = ppa_queue_watermark_margin_ms(&p->audio_watermark);
		if (ppa_queue_watermark_is_low(&p->audio_watermark) ||
		    (!ppa_queue_watermark_is_low(&p->video_watermark) &&
		     audio_margin < video_margin))
			selected = &p->audio_watermark;
	}
	ppa_queue_watermark_schedule_reader(selected, p->reader);
}

static void mp4_read_refresh_watermarks(struct mp4_read_struct *p)
{
	if (p == 0)
		return;
	ppa_queue_watermark_refresh(&p->video_watermark,
	                            p->video_queue,
	                            p->video_queue_front,
	                            p->video_queue_size,
	                            MP4_VIDEO_QUEUE_MAX);
	ppa_queue_watermark_refresh(&p->audio_watermark,
	                            p->audio_queue,
	                            p->audio_queue_front,
	                            p->audio_queue_size,
	                            MP4_AUDIO_QUEUE_MAX);
	mp4_read_schedule_readers(p);
}

static void mp4_read_init_watermarks(struct mp4_read_struct *p,
                                     uint32_t maximum_video_packet,
                                     uint32_t maximum_audio_packet)
{
	uint32_t video_ceiling;
	uint32_t audio_ceiling;

	if (p == 0)
		return;
	video_ceiling = ppa_hardware_has_64mb_ram() ?
	                2U * 1024U * 1024U : 1U * 1024U * 1024U;
	audio_ceiling = ppa_hardware_has_64mb_ram() ?
	                768U * 1024U : 384U * 1024U;
	video_ceiling = mp4_read_byte_ceiling(video_ceiling,
	                                      maximum_video_packet);
	audio_ceiling = mp4_read_byte_ceiling(audio_ceiling,
	                                      maximum_audio_packet);

	p->video_trim_floor = MP4_VIDEO_TRIM_FLOOR;
	p->audio_trim_floor = MP4_AUDIO_TRIM_FLOOR;
	ppa_queue_watermark_init(&p->video_watermark,
	                         150U, 400U, video_ceiling, 4U,
	                         mp4_read_nominal_ms(p->file.video_scale,
	                                             p->file.video_rate));
	ppa_queue_watermark_init(&p->audio_watermark,
	                         200U, 450U, audio_ceiling, 2U,
	                         mp4_read_nominal_ms(p->file.audio_resample_scale,
	                                             p->file.audio_rate));
}

static void mp4_read_reset_queue_policy(struct mp4_read_struct *p)
{
	if (p == 0)
		return;
	ppa_queue_watermark_reset(&p->video_watermark);
	ppa_queue_watermark_reset(&p->audio_watermark);
	mp4_read_refresh_watermarks(p);
}

static void mp4_read_trim_packet_pools(struct mp4_read_struct *p)
{
	if (p == 0)
		return;
	ppa_packet_pool_trim_free(&g_mp4_video_packet_pool,
	                          p->video_trim_floor, 0);
	ppa_packet_pool_trim_free(&g_mp4_audio_packet_pool,
	                          p->audio_trim_floor, 0);
}

static void mp4_read_clear_queues_at_safe_point(struct mp4_read_struct *p)
{
	if (p == 0)
		return;
	clear_mp4_read_queue(p->audio_queue,
	                     &p->audio_queue_size,
	                     &p->audio_queue_front,
	                     &p->audio_queue_rear,
	                     MP4_AUDIO_QUEUE_MAX);
	clear_mp4_read_queue(p->video_queue,
	                     &p->video_queue_size,
	                     &p->video_queue_front,
	                     &p->video_queue_rear,
	                     MP4_VIDEO_QUEUE_MAX);
	mp4_read_reset_queue_policy(p);
	mp4_read_trim_packet_pools(p);
}

void mp4_read_set_video_reorder_requirement(struct mp4_read_struct *p,
                                             unsigned int reorder_frames)
{
	uint32_t minimum_items;

	if (p == 0)
		return;
	minimum_items = reorder_frames > UINT_MAX - 2U ? UINT_MAX :
	                reorder_frames + 2U;
	if (minimum_items < 4U)
		minimum_items = 4U;
	ppa_queue_watermark_set_minimum_items(&p->video_watermark,
	                                      minimum_items,
	                                      MP4_VIDEO_QUEUE_MAX);
	mp4_read_refresh_watermarks(p);
}

static void zero_set_mp4_read_output_struct(struct mp4_read_output_struct *packet)
{
	ppa_media_packet_reset(packet);
}

void mp4_read_release_output(struct mp4_read_output_struct *packet)
{
	ppa_media_packet_release(packet);
}

static void mp4_read_prepare_packet(struct mp4_read_output_struct *packet,
                                    struct ppa_packet_pool *pool,
                                    struct ppa_packet_slot *slot,
                                    unsigned int size)
{
	ppa_media_packet_prepare(packet, pool, slot, size);
}

static unsigned int mp4_read_max_packet_size_for_track(struct mp4_read_struct *p, int track_id) {
	unsigned int i;
	unsigned int max_size = 1;

	for (i = 0; i < p->file.sample_count; i++) {
		int sample_track = (int)((p->file.samples[i].sample_index & 0xFF000000) >> 24);

		if (sample_track == track_id && p->file.samples[i].sample_size > max_size)
			max_size = p->file.samples[i].sample_size;
	}

	return max_size;
}

static int mp4_read_is_audio_track(struct mp4_read_struct *p, int track_id) {
	unsigned int i;

	for (i = 0; i < (unsigned int)p->file.audio_tracks; i++) {
		if (p->file.audio_track_ids[i] == track_id)
			return 1;
	}

	return 0;
}

static unsigned int mp4_read_max_audio_packet_size(struct mp4_read_struct *p) {
	unsigned int i;
	unsigned int max_size = 1;

	for (i = 0; i < p->file.sample_count; i++) {
		int sample_track = (int)((p->file.samples[i].sample_index & 0xFF000000) >> 24);

		if (mp4_read_is_audio_track(p, sample_track) &&
		    p->file.samples[i].sample_size > max_size) {
			max_size = p->file.samples[i].sample_size;
		}
	}

	return max_size;
}

static int in_mp4_read_queue(struct mp4_read_output_struct *queue,
                             unsigned int *queue_size,
                             unsigned int *queue_rear,
                             unsigned int queue_max,
                             struct mp4_read_output_struct *item)
{
	return ppa_media_packet_ring_push(queue, queue_size, queue_rear,
	                                  queue_max, item);
}

static int out_mp4_read_queue(struct mp4_read_output_struct *queue,
                              unsigned int *queue_size,
                              unsigned int *queue_front,
                              unsigned int queue_max,
                              struct mp4_read_output_struct *item)
{
	return ppa_media_packet_ring_pop(queue, queue_size, queue_front,
	                                 queue_max, item);
}

static void clear_mp4_read_queue(struct mp4_read_output_struct *queue,
                                 unsigned int *queue_size,
                                 unsigned int *queue_front,
                                 unsigned int *queue_rear,
                                 unsigned int queue_max)
{
	ppa_media_packet_ring_clear(queue, queue_size, queue_front, queue_rear,
	                            queue_max);
}

void mp4_read_safe_constructor(struct mp4_read_struct *p) {
	mp4_file_safe_constructor(&p->file);

	p->reader = 0;
	p->io_pump = 0;
	
	p->current_audio_track = 0;
	
	p->current_sample = 0;
	
	p->v_reader = 0;
	p->a_reader = 0;
	p->current_video_sample = 0;
	p->current_audio_sample = 0;
	
	int i;
	for(i=0; i<MP4_VIDEO_QUEUE_MAX; i++)
		zero_set_mp4_read_output_struct(&(p->video_queue[i]));
		
	p->video_queue_front = 0;
	p->video_queue_rear = 0;
	p->video_queue_size = 0;
		
	for(i=0; i<MP4_AUDIO_QUEUE_MAX; i++)
		zero_set_mp4_read_output_struct(&(p->audio_queue[i]));
	
	p->audio_queue_front = 0;
	p->audio_queue_rear = 0;
	p->audio_queue_size = 0;
	memset(&p->video_watermark, 0, sizeof(p->video_watermark));
	memset(&p->audio_watermark, 0, sizeof(p->audio_watermark));
	p->video_trim_floor = MP4_VIDEO_TRIM_FLOOR;
	p->audio_trim_floor = MP4_AUDIO_TRIM_FLOOR;

}

void mp4_read_close(struct mp4_read_struct *p) {
	int i;

	mp4_file_close(&p->file);

	if (p->reader)
		buffered_reader_close(p->reader);

	if (p->v_reader)
		buffered_reader_close(p->v_reader);

	if (p->a_reader)
		buffered_reader_close(p->a_reader);

	if (p->io_pump)
		ppa_io_pump_destroy(p->io_pump);

	for (i = 0; i < MP4_VIDEO_QUEUE_MAX; i++) {
		mp4_read_release_output(&p->video_queue[i]);
	}

	for (i = 0; i < MP4_AUDIO_QUEUE_MAX; i++) {
		mp4_read_release_output(&p->audio_queue[i]);
	}

	ppa_packet_pool_close(&g_mp4_video_packet_pool);
	ppa_packet_pool_close(&g_mp4_audio_packet_pool);

	mp4_read_safe_constructor(p);
}

int mp4_is_subtitle_track(struct mp4_read_struct *p, int track_id, int* index) {
	int i;
	for(i=0; i<p->file.subtitle_tracks; i++) {
		if ( p->file.subtitle_track_ids[i] == track_id ) {
			*index = i;
			return 1;
		}
	}
	return 0;
}

static int mp4_get_subtitle_frame_time(struct mp4_read_struct *p, int subtitle_index, unsigned int track_sample,
	uint64_t* timecode, uint64_t* duration) {
	int i;
	uint64_t ticks = 0;
	if (!p || !timecode || !duration || subtitle_index < 0 ||
	    subtitle_index >= p->file.subtitle_tracks ||
	    !p->file.subtitle_track_time[subtitle_index]) return 0;
	for (i = 0; i < p->file.subtitle_track_time_count[subtitle_index]; ++i) {
		unsigned int count = p->file.subtitle_track_time[subtitle_index][2*i];
		unsigned int delta = p->file.subtitle_track_time[subtitle_index][2*i+1];
		if (track_sample < count) {
			*timecode = ticks + (uint64_t)track_sample * delta;
			*duration = delta;
			return 1;
		}
		ticks += (uint64_t)count * delta;
		track_sample -= count;
	}
	return 0;
}

char *mp4_read_fill_buffer_not_interlace(struct mp4_read_struct *p, int track_id) {
	int video_track = p->file.video_track_id;
	int audio_track = p->file.audio_track_ids[p->current_audio_track];
	int current_track = track_id;
	int track;
	int tmp;
	unsigned int track_sample;
	uint64_t timestamp;

	if (current_track == video_track) {
		struct mp4_read_output_struct packet;
		struct ppa_packet_slot *slot;

		if (p->current_video_sample >= p->file.sample_count)
			return "mp4_read_fill_buffer: eof";

		track = (int)((p->file.samples[p->current_video_sample].sample_index & 0xFF000000) >> 24);
		if (track != video_track)
			return "mp4_read_fill_buffer: eof";

		zero_set_mp4_read_output_struct(&packet);

		slot = ppa_packet_pool_acquire(&g_mp4_video_packet_pool,
		                               p->file.samples[p->current_video_sample].sample_size);
		if (slot == 0) {
			return "mp4_read_fill_buffer: can not acquire video packet buffer";
		}

		mp4_read_prepare_packet(&packet,
		                        &g_mp4_video_packet_pool,
		                        slot,
		                        p->file.samples[p->current_video_sample].sample_size);

		if (!mp4_read_sample_exact(p->v_reader,
		                           p->file.samples[p->current_video_sample].file_offset,
		                           packet.data, packet.size)) {
			mp4_read_release_output(&packet);
			return("mp4_read_fill_buffer: can not read data");
		}

		track_sample = p->file.samples[p->current_video_sample].sample_index & 0x00FFFFFF;

		timestamp = mp4_file_video_timestamp_ms(&p->file, track_sample);

		packet.timestamp = (int)timestamp;
		packet.slot->timestamp = packet.timestamp;

		tmp = in_mp4_read_queue(p->video_queue,
		                        &p->video_queue_size,
		                        &p->video_queue_rear,
		                        MP4_VIDEO_QUEUE_MAX,
		                        &packet);
		if (tmp == 0) {
			mp4_read_release_output(&packet);
			return("mp4_read_fill_buffer: video queue is full");
		}

		p->current_video_sample++;
		mp4_read_refresh_watermarks(p);
	}
	else {
		struct mp4_read_output_struct packet;
		struct ppa_packet_slot *slot;

		if (p->current_audio_sample >= p->file.sample_count)
			return "mp4_read_fill_buffer: eof";

		track = (int)((p->file.samples[p->current_audio_sample].sample_index & 0xFF000000) >> 24);
		if (track != audio_track)
			return "mp4_read_fill_buffer: eof";

		zero_set_mp4_read_output_struct(&packet);

		slot = ppa_packet_pool_acquire(&g_mp4_audio_packet_pool,
		                               p->file.samples[p->current_audio_sample].sample_size);
		if (slot == 0) {
			return "mp4_read_fill_buffer: can not acquire audio packet buffer";
		}

		mp4_read_prepare_packet(&packet,
		                        &g_mp4_audio_packet_pool,
		                        slot,
		                        p->file.samples[p->current_audio_sample].sample_size);

		if (!mp4_read_sample_exact(p->a_reader,
		                           p->file.samples[p->current_audio_sample].file_offset,
		                           packet.data, packet.size)) {
			mp4_read_release_output(&packet);
			return("mp4_read_fill_buffer: can not read data");
		}

		track_sample = p->file.samples[p->current_audio_sample].sample_index & 0x00FFFFFF;

		timestamp = mp4_file_audio_timestamp_ms(&p->file,
			p->current_audio_track, track_sample);

		packet.timestamp = (int)timestamp;
        if (p->file.audio_formats[p->current_audio_track].mp4_timeline) {
            packet.audio_flags = 1;
            packet.audio_sample_position = mp4_file_audio_sample_position(&p->file, p->current_audio_track, track_sample);
            packet.audio_duration = mp4_file_audio_sample_duration(&p->file, p->current_audio_track, track_sample);
        }
		packet.slot->timestamp = packet.timestamp;

		tmp = in_mp4_read_queue(p->audio_queue,
		                        &p->audio_queue_size,
		                        &p->audio_queue_rear,
		                        MP4_AUDIO_QUEUE_MAX,
		                        &packet);
		if (tmp == 0) {
			mp4_read_release_output(&packet);
			return("mp4_read_fill_buffer: audio queue is full");
		}

		p->current_audio_sample++;
		mp4_read_refresh_watermarks(p);
	}

	return(0);
}

void mp4_handle_subtitle_frame(struct mp4_read_struct *p, int subtitle_index, unsigned int track_sample, unsigned int sample_size) {
	
	int64_t current_pos = buffered_reader_position64(p->reader);

	/* A failed cursor query must not become a huge unsigned seek. The sample is
	 * skipped safely; the caller's validated table offset will reposition the
	 * next iteration. */
	if (current_pos < 0)
		return;

	if ( sample_size > 2 ) {
		
		char trackname[32];
		memset(trackname, 0, 32);
		snprintf(trackname, sizeof(trackname), "mp4 subtitle track(%d)", p->file.subtitle_track_ids[subtitle_index]);
		struct subtitle_parse_struct *cur_parser = 0;
		int i,j;
		for(i=0; i < MAX_SUBTITLES; i++) {
			if (strcmp(trackname, subtitle_parser[i].filename) == 0 ) {
				cur_parser = &subtitle_parser[i];
				break;
			}
		}

		if ( cur_parser ) {
			struct subtitle_frame_struct* frame = (struct subtitle_frame_struct*)malloc_64( sizeof(struct subtitle_frame_struct) );
			if (frame) {
				uint8_t text_header[2];
				unsigned int text_size;
				subtitle_frame_safe_constructor(frame);
				{
					uint64_t timecode;
					uint64_t duration;
					uint64_t sub_scale;
					uint64_t denom;
					uint64_t start_num;
					uint64_t end_num;

					if (!mp4_get_subtitle_frame_time(p,
					                                 subtitle_index,
					                                 track_sample,
					                                 &timecode,
					                                 &duration)) {
						free_64(frame);
						goto subtitle_done;
					}

					sub_scale = p->file.subtitle_track_time_scale[subtitle_index];
					if (sub_scale == 0)
						sub_scale = 1000;

					/*
					* MP4 subtitle stts values are in the subtitle track's timescale.
					*
					* Convert subtitle time units -> seconds -> video frame number:
					*
					* frame = subtitle_time * video_rate / (subtitle_time_scale * video_scale)
					*/
					denom = sub_scale * (uint64_t)p->file.video_scale;
					if (!denom || !p->file.video_rate ||
					    duration > UINT64_MAX - timecode ||
					    timecode + duration >
					    (UINT64_MAX - denom / 2) / p->file.video_rate) {
						free_64(frame);
						goto subtitle_done;
					}

					start_num = timecode * (uint64_t)p->file.video_rate;
					end_num = (timecode + duration) * (uint64_t)p->file.video_rate;
					if ((end_num + denom / 2) / denom >= UINT_MAX) {
						free_64(frame);
						goto subtitle_done;
					}

					frame->p_start_frame = (unsigned int)((start_num + denom / 2) / denom);
					frame->p_end_frame = (unsigned int)((end_num + denom / 2) / denom);

					if (frame->p_end_frame <= frame->p_start_frame)
						frame->p_end_frame = frame->p_start_frame + 1;

				}
				
				frame->p_num_lines = 0;
				
				memset(frame->p_string, 0, max_subtitle_string);
				
				/* tx3g/text samples begin with a big-endian text byte count;
				 * trailing style atoms are not subtitle text. */
				if (!buffered_reader_read_exact(p->reader, text_header, 2)) {
					free_64(frame);
					goto subtitle_done;
				}
				text_size = ((unsigned int)text_header[0] << 8) | text_header[1];
				if (text_size > sample_size - 2U) {
					free_64(frame);
					goto subtitle_done;
				}
				if (text_size > max_subtitle_string - 1U)
					text_size = max_subtitle_string - 1U;
				if (text_size && !buffered_reader_read_exact(p->reader, frame->p_string, text_size)) {
					free_64(frame);
					goto subtitle_done;
				}
				
				i = 0;
				j = 0;
				char c = 0;
				int linelen = strlen(frame->p_string);
				if ( linelen > 0 ) {
					frame->p_num_lines++;
					while( i < linelen && j<max_subtitle_string-1) {
						c = frame->p_string[i++];
						if ( c == '\n' || c == '\r' ) {
							frame->p_string[j++]='\n';
							frame->p_num_lines++;
							while(i < linelen && (frame->p_string[i] == '\n' || frame->p_string[i] == '\r') ) i++;
						}
						else
							frame->p_string[j++]=c;
					}
				}
				frame->p_string[j] = '\0';

				subtitle_parse_add_frame( cur_parser, cur_parser->p_cur_sub_frame, frame);
			}
		}
	}
	
subtitle_done:
	(void)buffered_reader_seek64(p->reader,
	                             (uint64_t)current_pos + sample_size);
	
}

char *mp4_read_fill_buffer_interlace(struct mp4_read_struct *p, int track_id) {
	int video_track = p->file.video_track_id;
	int audio_track = p->file.audio_track_ids[p->current_audio_track];
	int current_track = track_id;
	int track;
	int tmp;
	unsigned int track_sample;
	uint64_t timestamp;
	int subtitle_index;

	while (1) {
		if (p->current_sample >= p->file.sample_count)
			return("mp4_read_fill_buffer: eof");

		track =
			(int)((p->file.samples[p->current_sample].sample_index &
			       0xFF000000) >> 24);

		/* Metadata indices let audio-only skip AVC/subtitle bytes entirely. */
		if (ppa_session_audio_only() && track != audio_track) {
			p->current_sample++;
			continue;
		}
		if (current_track == video_track && track == audio_track &&
		    ppa_queue_watermark_target_reached(&p->audio_watermark))
			return MP4_READ_VIDEO_AUDIO_BACKPRESSURE;
		if (current_track == audio_track && track == video_track &&
		    ppa_queue_watermark_target_reached(&p->video_watermark))
			return MP4_READ_AUDIO_VIDEO_BACKPRESSURE;

		if (buffered_reader_position64(p->reader) !=
		        (int64_t)p->file.samples[p->current_sample].file_offset &&
		    buffered_reader_seek64(p->reader,
		        p->file.samples[p->current_sample].file_offset) < 0)
			return "mp4_read_fill_buffer: sample seek failed";

		if (mp4_is_subtitle_track(p, track, &subtitle_index)) {
			track_sample =
				p->file.samples[p->current_sample].sample_index &
				0x00FFFFFF;

			mp4_handle_subtitle_frame(p,
			                          subtitle_index,
			                          track_sample,
			                          p->file.samples[p->current_sample].sample_size);

			p->current_sample++;
		}
		else if (track == video_track || track == audio_track) {
			struct mp4_read_output_struct packet;
			struct ppa_packet_pool *pool;
			struct ppa_packet_slot *slot;

			zero_set_mp4_read_output_struct(&packet);

			if (track == video_track)
				pool = &g_mp4_video_packet_pool;
			else
				pool = &g_mp4_audio_packet_pool;

			slot = ppa_packet_pool_acquire(pool,
			                               p->file.samples[p->current_sample].sample_size);

			if (slot == 0) {
				if (track == video_track)
					return("mp4_read_fill_buffer: can not acquire video packet buffer");
				else
					return("mp4_read_fill_buffer: can not acquire audio packet buffer");
			}

			mp4_read_prepare_packet(&packet,
			                        pool,
			                        slot,
			                        p->file.samples[p->current_sample].sample_size);

			if (!mp4_read_sample_exact(p->reader,
			                           p->file.samples[p->current_sample].file_offset,
			                           packet.data, packet.size)) {
				mp4_read_release_output(&packet);
				return("mp4_read_fill_buffer: can not read data");
			}

			track_sample =
				p->file.samples[p->current_sample].sample_index &
				0x00FFFFFF;

			if (track == video_track) {
				timestamp = mp4_file_video_timestamp_ms(&p->file, track_sample);

				packet.timestamp = (int)timestamp;
				packet.slot->timestamp = packet.timestamp;

				tmp = in_mp4_read_queue(p->video_queue,
				                        &p->video_queue_size,
				                        &p->video_queue_rear,
				                        MP4_VIDEO_QUEUE_MAX,
				                        &packet);
			}
			else {
				timestamp = mp4_file_audio_timestamp_ms(&p->file,
					p->current_audio_track, track_sample);

				packet.timestamp = (int)timestamp;
        if (p->file.audio_formats[p->current_audio_track].mp4_timeline) {
            packet.audio_flags = 1;
            packet.audio_sample_position = mp4_file_audio_sample_position(&p->file, p->current_audio_track, track_sample);
            packet.audio_duration = mp4_file_audio_sample_duration(&p->file, p->current_audio_track, track_sample);
        }
				packet.slot->timestamp = packet.timestamp;

				tmp = in_mp4_read_queue(p->audio_queue,
				                        &p->audio_queue_size,
				                        &p->audio_queue_rear,
				                        MP4_AUDIO_QUEUE_MAX,
				                        &packet);
			}

			if (tmp == 0) {
				mp4_read_release_output(&packet);

				if (track == video_track)
					return("mp4_read_fill_buffer: video queue is full");
				else
					return("mp4_read_fill_buffer: audio queue is full");
			}

			p->current_sample++;
			mp4_read_refresh_watermarks(p);

			if (track == current_track)
				return(0);
		}
		else {
			/* Unknown/disabled media sample. The next loop positions from the
			 * validated table offset, so no relative seek can overflow. */
			p->current_sample++;
		}
	}

	return(0);
}

char *mp4_read_open(struct mp4_read_struct *p, char *s) {
	char *result;
	uint32_t maximum_video_packet;
	uint32_t maximum_audio_packet;

	mp4_read_safe_constructor(p);

	result = mp4_file_open(&p->file, s);

	if (result != 0) {
		mp4_read_close(p);
		return(result);
	}

	maximum_video_packet =
		mp4_read_max_packet_size_for_track(p, p->file.video_track_id);
	maximum_audio_packet = mp4_read_max_audio_packet_size(p);
    if (maximum_audio_packet > PPU_AUDIO_MAX_PACKET_BYTES) {
        mp4_read_close(p);
        return "audio: MP4 packet exceeds workspace limit";
    }
	mp4_read_init_watermarks(p, maximum_video_packet, maximum_audio_packet);

	if (!ppa_packet_pool_init(&g_mp4_video_packet_pool,
	                          MP4_VIDEO_QUEUE_MAX + 2,
	                          p->video_trim_floor)) {
		mp4_read_close(p);
		return("mp4_read_open: can not init video packet pool");
	}

	ppa_packet_pool_set_kind(&g_mp4_video_packet_pool,
	                               PPA_PACKET_POOL_KIND_VIDEO);

	if (!ppa_packet_pool_init(&g_mp4_audio_packet_pool,
	                          MP4_AUDIO_QUEUE_MAX + 2,
	                          p->audio_trim_floor)) {
		mp4_read_close(p);
		return("mp4_read_open: can not init audio packet pool");
	}

	ppa_packet_pool_set_kind(&g_mp4_audio_packet_pool,
	                               PPA_PACKET_POOL_KIND_AUDIO);

	p->io_pump = ppa_io_pump_create();
	if (p->io_pump == 0) {
		mp4_read_close(p);
		return "mp4_read_open: can not create I/O pump";
	}

	if (p->file.is_not_interlace) {

		p_mp4_read_fill_buffer = mp4_read_fill_buffer_not_interlace;

		p->v_reader = buffered_reader_open_with_pump_at(
			s, 64 * 1024, 0, 0x30, p->io_pump,
			MP4_IO_STREAM_VIDEO, p->file.first_video_offset);

		if (!p->v_reader) {
			mp4_read_close(p);
			return("mp4_read_open: can't open file(video)");
		}

		p->a_reader = buffered_reader_open_with_pump_at(
			s, 32 * 1024, 0, 0x30, p->io_pump,
			MP4_IO_STREAM_AUDIO, p->file.first_audio_offset);

		if (!p->a_reader) {
			mp4_read_close(p);
			return("mp4_read_open: can't open file(audio)");
		}

		p->current_video_sample = p->file.first_video_sample;
		p->current_audio_sample = p->file.first_audio_sample;

	}
	else {

		p_mp4_read_fill_buffer = mp4_read_fill_buffer_interlace;

		p->reader = buffered_reader_open_with_pump_at(
			s, 96 * 1024, 0, 0x30, p->io_pump,
			MP4_IO_STREAM_INTERLEAVED, p->file.first_video_offset);

		if (!p->reader) {
			mp4_read_close(p);
			return("mp4_read_open: can't open file");
		}

		p->current_sample = p->file.first_video_sample;

	}

	mp4_read_refresh_watermarks(p);

	return(0);
}

char *mp4_read_fill_buffer(struct mp4_read_struct *p, int track_id) {
	return p_mp4_read_fill_buffer(p, track_id);
}

void mp4_do_seek(struct mp4_read_struct *p, struct mp4_index_struct* index) {
	if (p == 0 || index == 0)
		return;
	if (p->file.is_not_interlace) {
		(void)buffered_reader_seek64(p->v_reader, index->offset);
		p->current_video_sample = index->sample_index;

		/* Audio seek position is derived from the selected keyframe timestamp,
		 * not from a nominal video frame count.  This preserves A/V alignment
		 * for VFR files.  The validated per-sample file offset avoids legacy
		 * cumulative-size arithmetic across damaged/noncontiguous chunks. */
		if (p->file.first_audio_sample < p->file.sample_count &&
		    p->current_audio_track < (unsigned int)p->file.audio_tracks) {
			unsigned int audio_in_track = mp4_file_audio_sample_at_ms(
				&p->file, p->current_audio_track, index->timestamp +
                (unsigned int)((uint64_t)p->file.audio_formats[p->current_audio_track].delay_samples * 1000ULL /
                               p->file.audio_formats[p->current_audio_track].rate));
			unsigned int available =
				p->file.audio_sample_count[p->current_audio_track];
			unsigned int next_audio_sample;
			if (available == 0 || available > p->file.sample_count -
			    p->file.first_audio_sample)
				return;
			if (audio_in_track >= available)
				audio_in_track = available - 1U;
			next_audio_sample = p->file.first_audio_sample +
				(unsigned int)audio_in_track;
			(void)buffered_reader_seek64(
				p->a_reader,
				p->file.samples[next_audio_sample].file_offset);
			p->current_audio_sample = next_audio_sample;
		}
	}
	else {
		(void)buffered_reader_seek64(p->reader, index->offset);
		p->current_sample = index->sample_index;
	}
}

char *mp4_read_seek(struct mp4_read_struct *p, int timestamp, int last_timestamp) {
	unsigned int lo = 0;
	unsigned int hi;
	unsigned int selected;
	struct mp4_index_struct *index;
	char *result = 0;

	if (p == 0 || p->file.index_count == 0 || p->file.indexes == 0)
		return("mp4_read_seek: no sync sample index");
	if (timestamp < 0)
		timestamp = 0;
	if (last_timestamp < 0)
		last_timestamp = 0;

    /* Start video and interleaved audio early enough for Opus recovery. The
     * presentation controller retains the original requested seek target. */
    {
        const struct ppu_audio_format *f = &p->file.audio_formats[p->current_audio_track];
        unsigned int preroll_ms = f->preroll_frames ?
            (unsigned int)((uint64_t)f->preroll_frames * 1000ULL / f->rate) + 120U : 0;
        timestamp = (unsigned int)timestamp > preroll_ms ? timestamp - (int)preroll_ms : 0;
    }

	/* lower_bound(target) on the monotonic sync timestamp table. Always begin
	 * from the last sync sample at/before the target. Predictive AVC needs this
	 * preroll in both directions; the show thread decodes it invisibly and only
	 * acknowledges the seek when presentation reaches the requested timestamp. */
	hi = p->file.index_count;
	while (lo < hi) {
		unsigned int mid = lo + (hi - lo) / 2U;
		if ((uint64_t)p->file.indexes[mid].timestamp < (uint64_t)(unsigned int)timestamp)
			lo = mid + 1U;
		else
			hi = mid;
	}

	(void)last_timestamp;
	if (lo < p->file.index_count &&
	    p->file.indexes[lo].timestamp == (unsigned int)timestamp)
		selected = lo;
	else
		selected = lo == 0U ? 0U : lo - 1U;
	index = p->file.indexes + selected;
	mp4_do_seek(p, index);

	mp4_read_clear_queues_at_safe_point(p);

	while (p->video_queue_size == 0U) {
		result = mp4_read_fill_buffer(p, p->file.video_track_id);
		if (result)
			return result;
	}
	return 0;
}

char *mp4_read_keyframe_forward(struct mp4_read_struct *p, int keyframes) {
	unsigned int i;
	unsigned int current_sample;
	struct mp4_index_struct* index;
	if (p == 0 || p->file.index_count == 0 || p->file.indexes == 0)
		return("mp4_read_keyframe_forward: no sync sample index");
	if (p->file.is_not_interlace) {
		current_sample = p->current_video_sample;
	}
	else {
		current_sample = p->current_sample;
	}
	if (keyframes < 1)
		keyframes = 1;
	index = p->file.indexes + (p->file.index_count - 1U);
	for(i=0; i < p->file.index_count; i++) {
		index = p->file.indexes + i;
		if ( index->sample_index > current_sample )
			keyframes--;
		if(keyframes == 0)
			break;
	}
	
	mp4_do_seek(p, index);
	
	mp4_read_clear_queues_at_safe_point(p);
	char* result = 0;
	while(1) {
		result = mp4_read_fill_buffer(p, p->file.video_track_id);
		if (result) {
			return (result);
		}
		if ( p->video_queue_size > 0 )
			break; 
	}
	return(0);
}

char *mp4_read_keyframe_backward(struct mp4_read_struct *p, int keyframes) {
	int i;
	unsigned int current_sample;
	if (p == 0 || p->file.index_count == 0 || p->file.indexes == 0)
		return("mp4_read_keyframe_backward: no sync sample index");
	if (keyframes < 1)
		keyframes = 1;
	if (p->file.is_not_interlace) {
		current_sample = p->current_video_sample;
	}
	else {
		current_sample = p->current_sample;
	}
	struct mp4_index_struct* index = 0;
	for(i=p->file.index_count-1; i >= 0; i--) {
		index = p->file.indexes + i;
		if ( index->sample_index < current_sample )
			keyframes--;
		if(keyframes == 0)
			break;
	}
	
	mp4_do_seek(p, index);
	
	mp4_read_clear_queues_at_safe_point(p);
	char* result = 0;
	while(1) {
		result = mp4_read_fill_buffer(p, p->file.video_track_id);
		if (result) {
			return (result);
		}
		if ( p->video_queue_size > 0 )
			break; 
	}
	return(0);
}

char *mp4_read_get_video(struct mp4_read_struct *p,
                         struct mp4_read_output_struct *output)
{
	if (p == 0 || output == 0)
		return "mp4_read_get_video: invalid argument";

	if (p->video_queue_size == 0U) {
		char *res;

		if (!p->file.is_not_interlace && p->file.audio_tracks > 0 &&
		    ppa_queue_watermark_target_reached(&p->audio_watermark))
			return MP4_READ_VIDEO_AUDIO_BACKPRESSURE;
		res = mp4_read_fill_buffer(p, p->file.video_track_id);
		if (res)
			return res;
		if (p->video_queue_size == 0U)
			return "mp4_read_get_video: video queue is empty";
	}

	if (!out_mp4_read_queue(p->video_queue,
	                        &p->video_queue_size,
	                        &p->video_queue_front,
	                        MP4_VIDEO_QUEUE_MAX,
	                        output))
		return "mp4_read_get_video: video queue pop failed";
	mp4_read_refresh_watermarks(p);
	return 0;
}

char *mp4_read_get_audio(struct mp4_read_struct *p,
                         unsigned int audio_stream,
                         struct mp4_read_output_struct *output)
{
	if (p == 0 || output == 0 || audio_stream >= (unsigned int)p->file.audio_tracks)
		return "mp4_read_get_audio: invalid audio stream";

	if (p->current_audio_track != (int)audio_stream) {
		clear_mp4_read_queue(p->audio_queue,
		                     &p->audio_queue_size,
		                     &p->audio_queue_front,
		                     &p->audio_queue_rear,
		                     MP4_AUDIO_QUEUE_MAX);
		p->current_audio_track = (int)audio_stream;
		ppa_queue_watermark_reset(&p->audio_watermark);
		ppa_packet_pool_trim_free(&g_mp4_audio_packet_pool,
		                          p->audio_trim_floor, 0);
		mp4_read_refresh_watermarks(p);
	}

	if (p->audio_queue_size == 0U) {
		char *res;

		if (!ppa_session_audio_only() && !p->file.is_not_interlace &&
		    ppa_queue_watermark_target_reached(&p->video_watermark))
			return MP4_READ_AUDIO_VIDEO_BACKPRESSURE;
		res = mp4_read_fill_buffer(p, p->file.audio_track_ids[audio_stream]);
		if (res)
			return res;
		if (p->audio_queue_size == 0U)
			return "mp4_read_get_audio: audio queue is empty";
	}

	if (!out_mp4_read_queue(p->audio_queue,
	                        &p->audio_queue_size,
	                        &p->audio_queue_front,
	                        MP4_AUDIO_QUEUE_MAX,
	                        output))
		return "mp4_read_get_audio: audio queue pop failed";
	mp4_read_refresh_watermarks(p);
	return 0;
}
