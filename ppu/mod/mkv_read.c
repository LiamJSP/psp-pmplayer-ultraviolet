/* mkv_read.c
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

#include "mkv_read.h"
#include "common/ppa_playback_session.h"
#include "ebml_id.h"
#include "subtitle_parse.h"
#include "subtitle_text.h"
#include "common/libminiconv.h"
#include "../common/ppa_packet_pool.h"
#include "../common/ppa_time.h"
#include "../common/ppa_hardware_profile.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#ifndef MKV_MAX_LACES
#define MKV_MAX_LACES 32
#endif

#ifndef MKV_MAX_BLOCK_SIZE
#define MKV_MAX_BLOCK_SIZE (256U * 1024U)
#endif

#ifndef MKV_UNKNOWN_SIZE
#define MKV_UNKNOWN_SIZE UINT64_MAX
#endif

static int mkv_read_seek_abs64(buffered_reader_t *reader, uint64_t pos)
{
	if (reader == 0)
		return -1;
	return buffered_reader_seek64(reader, pos) < 0 ? -1 : 0;
}

static int mkv_master_subtract(uint64_t *size, uint64_t amount)
{
	if (size == 0)
		return 0;

	if (*size == MKV_UNKNOWN_SIZE)
		return 1;

	if (amount > *size) {
		*size = 0;
		return 0;
	}

	*size -= amount;
	return 1;
}

static uint64_t mkv_block_read_vint_uint_checked(const uint8_t *buffer,
                                                 uint64_t remaining,
                                                 int32_t *length)
{
	int32_t i;
	int32_t j;
	int32_t num_ffs = 0;
	int32_t len_mask = 0x80;
	uint64_t num;

	if (length)
		*length = 0;

	if (buffer == 0 || remaining == 0)
		return EBML_UINT_INVALID;

	num = *buffer++;

	for (i = 0; i < 8 && !(num & len_mask); i++)
		len_mask >>= 1;

	if (i >= 8)
		return EBML_UINT_INVALID;

	j = i + 1;

	if ((uint64_t)j > remaining)
		return EBML_UINT_INVALID;

	if (length)
		*length = j;

	num &= (len_mask - 1);

	if ((int32_t)num == len_mask - 1)
		num_ffs++;

	while (i--) {
		num = (num << 8) | *buffer++;

		if ((num & 0xFF) == 0xFF)
			num_ffs++;
	}

	if (j == num_ffs)
		return EBML_UINT_INVALID;

	return num;
}

static int64_t mkv_block_read_vint_int_checked(const uint8_t *buffer,
                                               uint64_t remaining,
                                               int32_t *length)
{
	uint64_t unum;
	int32_t l;
	int64_t bias;

	unum = mkv_block_read_vint_uint_checked(buffer, remaining, &l);
	if (unum == EBML_UINT_INVALID)
		return EBML_INT_INVALID;

	if (l < 1 || l > 8)
		return EBML_INT_INVALID;

	if (length)
		*length = l;

	if (l == 8)
		bias = 0x00FFFFFFFFFFFFFFLL;
	else
		bias = ((int64_t)1 << ((7 * l) - 1)) - 1;

	return (int64_t)unum - bias;
}

uint32_t mkv_ebml_read_id(buffered_reader_t *reader, int32_t *length);
uint64_t mkv_ebml_read_length(buffered_reader_t *reader, int32_t *length);

#ifndef MKV_RECOVERY_SCAN_LIMIT
#define MKV_RECOVERY_SCAN_LIMIT (256U * 1024U)
#endif

static int mkv_read_plausible_cluster_child(uint32_t id)
{
    switch (id) {
    case MATROSKA_ID_CLUSTERTIMECODE:
    case MATROSKA_ID_SIMPLEBLOCK:
    case MATROSKA_ID_BLOCKGROUP:
    case EBML_ID_CRC32:
    case EBML_ID_VOID:
        return 1;
    default:
        return 0;
    }
}

/* Bounded, fail-soft recovery for damaged inter-cluster data. The scanner is
 * used only after a structural parse failure, never on the normal path. A
 * candidate must have a valid finite size within the signed-32-bit reader and
 * a plausible first child, which keeps random H.264 payload matches unlikely. */
static int mkv_read_resync_cluster(struct mkv_read_struct *p,
                                   unsigned int scan_limit,
                                   const char *reason)
{
    uint32_t rolling = 0;
    unsigned int seen = 0;
    int64_t start;
    int64_t end;
    int64_t file_length;

    if (p == 0 || p->reader == 0)
        return 0;
    start = buffered_reader_position64(p->reader);
    file_length = buffered_reader_length64(p->reader);
    if (start < 0 || file_length <= start)
        return 0;
    end = start + (int64_t)scan_limit;
    if (end < start || end > file_length)
        end = file_length;

    while (buffered_reader_position64(p->reader) < end) {
        uint8_t byte;
        int64_t candidate;
        uint64_t cluster_size;
        int32_t length_bytes = 0;
        int64_t child_pos;
        int32_t child_id_length = 0;
        uint32_t child_id;
        int64_t after_length;

        if (!buffered_reader_read_exact(p->reader, &byte, 1))
            break;
        rolling = (rolling << 8) | byte;
        if (seen < 4U)
            seen++;
        if (seen < 4U || rolling != MATROSKA_ID_CLUSTER)
            continue;

        candidate = buffered_reader_position64(p->reader) - 4;
        cluster_size = mkv_ebml_read_length(p->reader, &length_bytes);
        after_length = buffered_reader_position64(p->reader);
        if (cluster_size == EBML_UINT_INVALID || length_bytes <= 0 ||
            after_length < 0 || after_length > file_length ||
            cluster_size > (uint64_t)(file_length - after_length)) {
            if (buffered_reader_seek64(p->reader,
                                       (uint64_t)candidate + 1ULL) < 0)
                break;
            rolling = 0;
            seen = 0;
            continue;
        }

        child_pos = buffered_reader_position64(p->reader);
        child_id = mkv_ebml_read_id(p->reader, &child_id_length);
        if (child_id == EBML_ID_INVALID || child_id_length <= 0 ||
            !mkv_read_plausible_cluster_child(child_id)) {
            if (buffered_reader_seek64(p->reader,
                                       (uint64_t)candidate + 1ULL) < 0)
                break;
            rolling = 0;
            seen = 0;
            continue;
        }

        if (buffered_reader_seek64(p->reader, (uint64_t)child_pos) < 0)
            break;
        p->cluster_size = cluster_size;
        p->cluster_timecode = 0;
        p->blockgroup_size = 0;
        p->block_size = 0;
        return 1;
    }

    return 0;
}

static struct ppa_packet_pool g_mkv_video_packet_pool;
static struct ppa_packet_pool g_mkv_audio_packet_pool;

static void zero_set_mkv_read_output_struct(struct mkv_read_output_struct *packet)
{
	ppa_media_packet_reset(packet);
}

void mkv_read_release_output(struct mkv_read_output_struct *packet)
{
	ppa_media_packet_release(packet);
}

static void mkv_release_block_buffer(struct mkv_read_struct *p) {
	if (p->block_buffer != 0) {
		free_64(p->block_buffer);
		p->block_buffer = 0;
	}

	p->block_buffer_capacity = 0;
}

static char *mkv_ensure_block_buffer(struct mkv_read_struct *p, uint64_t required_size)
{
	void *new_buffer;

	if (required_size > MKV_MAX_BLOCK_SIZE + MKV_INPUT_PADDING)
		return "mkv_read_fill_buffer: block buffer too large";

	if (required_size > 0x7fffffffULL)
		return "mkv_read_fill_buffer: block buffer too large";

	if (p->block_buffer != 0 &&
	    p->block_buffer_capacity >= (unsigned int)required_size)
		return 0;

	new_buffer = malloc_64((unsigned int)required_size);
	if (new_buffer == 0)
		return "mkv_read_fill_buffer: can not malloc_64 block_buffer";

	if (p->block_buffer != 0)
		free_64(p->block_buffer);

	p->block_buffer = new_buffer;
	p->block_buffer_capacity = (unsigned int)required_size;

	return 0;
}

static int in_mkv_read_queue(struct mkv_read_output_struct *queue,
                             unsigned int *queue_size,
                             unsigned int *queue_rear,
                             unsigned int queue_max,
                             struct mkv_read_output_struct *item)
{
	return ppa_media_packet_ring_push(queue, queue_size, queue_rear,
	                                  queue_max, item);
}

static int out_mkv_read_queue(struct mkv_read_output_struct *queue,
                              unsigned int *queue_size,
                              unsigned int *queue_front,
                              unsigned int queue_max,
                              struct mkv_read_output_struct *item)
{
	return ppa_media_packet_ring_pop(queue, queue_size, queue_front,
	                                 queue_max, item);
}

static void clear_mkv_read_queue(struct mkv_read_output_struct *queue,
                                 unsigned int *queue_size,
                                 unsigned int *queue_front,
                                 unsigned int *queue_rear,
                                 unsigned int queue_max)
{
	ppa_media_packet_ring_clear(queue, queue_size, queue_front, queue_rear,
	                            queue_max);
}

#define MKV_IO_STREAM_INTERLEAVED 0U
#define MKV_VIDEO_TRIM_FLOOR      (64U * 1024U)
#define MKV_AUDIO_TRIM_FLOOR      (16U * 1024U)

static uint32_t mkv_read_nominal_ms(uint32_t scale, uint32_t rate)
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

static uint32_t mkv_read_byte_ceiling(uint32_t base)
{
	uint64_t minimum =
		2ULL * ((uint64_t)MKV_MAX_BLOCK_SIZE + 16ULL * 1024ULL);

	if (minimum < base)
		minimum = base;
	return minimum > UINT_MAX ? UINT_MAX : (uint32_t)minimum;
}

static void mkv_read_schedule_reader(struct mkv_read_struct *p)
{
	const struct PpaQueueWatermark *selected;
	uint32_t video_margin;
	uint32_t audio_margin;

	if (p == 0 || p->reader == 0)
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

static void mkv_read_refresh_watermarks(struct mkv_read_struct *p)
{
	if (p == 0)
		return;
	ppa_queue_watermark_refresh(&p->video_watermark,
	                            p->video_queue,
	                            p->video_queue_front,
	                            p->video_queue_size,
	                            MKV_VIDEO_QUEUE_MAX);
	ppa_queue_watermark_refresh(&p->audio_watermark,
	                            p->audio_queue,
	                            p->audio_queue_front,
	                            p->audio_queue_size,
	                            MKV_AUDIO_QUEUE_MAX);
	mkv_read_schedule_reader(p);
}

static void mkv_read_init_watermarks(struct mkv_read_struct *p)
{
	uint32_t video_ceiling;
	uint32_t audio_ceiling;

	if (p == 0)
		return;
	video_ceiling = ppa_hardware_has_64mb_ram() ?
	                2U * 1024U * 1024U : 1U * 1024U * 1024U;
	audio_ceiling = ppa_hardware_has_64mb_ram() ?
	                768U * 1024U : 384U * 1024U;
	video_ceiling = mkv_read_byte_ceiling(video_ceiling);
	audio_ceiling = mkv_read_byte_ceiling(audio_ceiling);

	p->video_trim_floor = MKV_VIDEO_TRIM_FLOOR;
	p->audio_trim_floor = MKV_AUDIO_TRIM_FLOOR;
	ppa_queue_watermark_init(&p->video_watermark,
	                         150U, 400U, video_ceiling, 4U,
	                         mkv_read_nominal_ms(p->file.video_scale,
	                                             p->file.video_rate));
	ppa_queue_watermark_init(&p->audio_watermark,
	                         200U, 450U, audio_ceiling, 2U,
	                         mkv_read_nominal_ms(p->file.audio_resample_scale,
	                                             p->file.audio_rate));
}

static void mkv_read_reset_queue_policy(struct mkv_read_struct *p)
{
	if (p == 0)
		return;
	ppa_queue_watermark_reset(&p->video_watermark);
	ppa_queue_watermark_reset(&p->audio_watermark);
	mkv_read_refresh_watermarks(p);
}

static void mkv_read_trim_packet_pools(struct mkv_read_struct *p)
{
	if (p == 0)
		return;
	ppa_packet_pool_trim_free(&g_mkv_video_packet_pool,
	                          p->video_trim_floor, 0);
	ppa_packet_pool_trim_free(&g_mkv_audio_packet_pool,
	                          p->audio_trim_floor, 0);
}

static void mkv_read_clear_queues_at_safe_point(struct mkv_read_struct *p)
{
	if (p == 0)
		return;
	clear_mkv_read_queue(p->audio_queue,
	                     &p->audio_queue_size,
	                     &p->audio_queue_front,
	                     &p->audio_queue_rear,
	                     MKV_AUDIO_QUEUE_MAX);
	clear_mkv_read_queue(p->video_queue,
	                     &p->video_queue_size,
	                     &p->video_queue_front,
	                     &p->video_queue_rear,
	                     MKV_VIDEO_QUEUE_MAX);
	mkv_release_block_buffer(p);
	p->block_size = 0;
	mkv_read_reset_queue_policy(p);
	mkv_read_trim_packet_pools(p);
}

void mkv_read_set_video_reorder_requirement(struct mkv_read_struct *p,
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
	                                      MKV_VIDEO_QUEUE_MAX);
	mkv_read_refresh_watermarks(p);
}

//static uint64_t mkv_read_be64(buffered_reader_t* reader) {
//	uint8_t data[8];
//	uint64_t result = 0;
//	int i;
//	
//	buffered_reader_read(reader, data, 8);
//	for (i = 0; i < 8; i++) {
//		result |= ((uint64_t)data[i]) << ((7 - i) * 8);
//	}
//
//	return result;
//}
//
//static uint32_t mkv_read_be32(buffered_reader_t* reader) {
//	uint8_t data[4];
//	uint32_t result = 0;
//	uint32_t a, b, c, d;
//	
//	buffered_reader_read(reader, data, 4);
//	a = (uint8_t)data[0];
//	b = (uint8_t)data[1];
//	c = (uint8_t)data[2];
//	d = (uint8_t)data[3];
//	result = (a<<24) | (b<<16) | (c<<8) | d;
//
//	return result;
//}
//
//static uint16_t mkv_read_be16(buffered_reader_t* reader) {
//	uint8_t data[2];
//	uint16_t result = 0;
//	uint16_t a, b;
//	
//	buffered_reader_read(reader, data, 2);
//	a = (uint8_t)data[0];
//	b = (uint8_t)data[1];
//	result = (a<<8) | b;
//
//	return result;
//}

static int g_mkv_reader_io_error = 0;

static int mkv_read_u8_checked(buffered_reader_t* reader, uint8_t *out)
{
	if (reader == 0 || out == 0) {
		g_mkv_reader_io_error = 1;
		return 0;
	}

	if (!buffered_reader_read_exact(reader, out, 1)) {
		g_mkv_reader_io_error = 1;
		*out = 0;
		return 0;
	}

	return 1;
}

uint8_t mkv_read_8(buffered_reader_t* reader)
{
	uint8_t result = 0;
	mkv_read_u8_checked(reader, &result);
	return result;
}

uint32_t mkv_ebml_read_id(buffered_reader_t* reader, int32_t *length)
{
	int32_t i;
	int32_t len_mask = 0x80;
	uint32_t id;
	uint8_t byte;

	g_mkv_reader_io_error = 0;

	if (!mkv_read_u8_checked(reader, &byte))
		return EBML_ID_INVALID;

	id = byte;

	for (i = 0; i < 4 && !(id & len_mask); i++)
		len_mask >>= 1;

	if (i >= 4)
		return EBML_ID_INVALID;

	if (length)
		*length = i + 1;

	while (i--) {
		if (!mkv_read_u8_checked(reader, &byte))
			return EBML_ID_INVALID;

		id = (id << 8) | byte;
	}

	return id;
}

uint64_t mkv_ebml_read_length(buffered_reader_t* reader, int32_t* length)
{
	int32_t i;
	int32_t j;
	int32_t num_ffs = 0;
	int32_t len_mask = 0x80;
	uint64_t len;
	uint8_t byte;

	g_mkv_reader_io_error = 0;

	if (!mkv_read_u8_checked(reader, &byte))
		return EBML_UINT_INVALID;

	len = byte;

	for (i = 0; i < 8 && !(len & len_mask); i++)
		len_mask >>= 1;

	if (i >= 8)
		return EBML_UINT_INVALID;

	j = i + 1;

	if (length)
		*length = j;

	len &= (len_mask - 1);

	if ((int)len == len_mask - 1)
		num_ffs++;

	while (i--) {
		if (!mkv_read_u8_checked(reader, &byte))
			return EBML_UINT_INVALID;

		len = (len << 8) | byte;

		if ((len & 0xFF) == 0xFF)
			num_ffs++;
	}

	if (j == num_ffs)
		return EBML_UINT_INVALID;

	return len;
}

uint64_t mkv_ebml_read_uint(buffered_reader_t* reader, uint64_t* length)
{
	uint64_t len;
	uint64_t value = 0;
	int32_t l;
	uint8_t byte;

	len = mkv_ebml_read_length(reader, &l);
	if (len == EBML_UINT_INVALID || len < 1 || len > 8)
		return EBML_UINT_INVALID;

	if (length)
		*length = len + l;

	while (len--) {
		if (!mkv_read_u8_checked(reader, &byte))
			return EBML_UINT_INVALID;

		value = (value << 8) | byte;
	}

	return value;
}

int64_t mkv_ebml_read_int(buffered_reader_t* reader, uint64_t* length)
{
	uint64_t bits = 0;
	int64_t value;
	uint64_t len;
	uint64_t bytes;
	int32_t l;
	uint8_t byte;

	len = mkv_ebml_read_length(reader, &l);
	if (len == EBML_UINT_INVALID || len < 1 || len > 8)
		return EBML_INT_INVALID;

	if (length)
		*length = len + l;

	bytes = len;
	while (len--) {
		if (!mkv_read_u8_checked(reader, &byte))
			return EBML_INT_INVALID;

		bits = (bits << 8) | byte;
	}
	if (bytes < 8 && (bits & (1ULL << (bytes * 8U - 1U))))
		bits |= UINT64_MAX << (bytes * 8U);
	memcpy(&value, &bits, sizeof(value));

	return value;
}

int32_t mkv_ebml_read_skip(buffered_reader_t* reader, uint64_t* length)
{
	uint64_t len;
	int32_t l;
	int64_t position;
	uint64_t target;

	len = mkv_ebml_read_length(reader, &l);
	if (len == EBML_UINT_INVALID)
		return 1;

	if (length)
		*length = len + l;

	position = buffered_reader_position64(reader);
	if (position < 0 || len > UINT64_MAX - (uint64_t)position)
		return 1;
	target = (uint64_t)position + len;

	if (buffered_reader_seek64(reader, target) < 0)
		return 1;

	return 0;
}
uint64_t mkv_ebml_read_vlen_uint(uint8_t* buffer, int32_t* length) {
	int32_t i, j, num_ffs = 0, len_mask = 0x80;
	uint64_t num;

	for (i=0, num=*buffer++; i<8 && !(num & len_mask); i++)
		len_mask >>= 1;
	if (i >= 8)
		return EBML_UINT_INVALID;
	j = i+1;
	if (length)
		*length = j;
	if ((int)(num &= (len_mask - 1)) == len_mask - 1)
		num_ffs++;
	while (i--) {
		num = (num << 8) | *buffer++;
		if ((num & 0xFF) == 0xFF)
			num_ffs++;
	}
	if (j == num_ffs)
		return EBML_UINT_INVALID;
	return num;
}

int64_t mkv_ebml_read_vlen_int(uint8_t* buffer, int32_t *length) {
	uint64_t unum;
	int32_t l;

	unum = mkv_ebml_read_vlen_uint (buffer, &l);
	if (unum == EBML_UINT_INVALID)
		return EBML_INT_INVALID;
	if (length)
		*length = l;

	return unum - ((1 << ((7 * l) - 1)) - 1);
}

void mkv_read_safe_constructor(struct mkv_read_struct *p) {
	mkv_file_safe_constructor(&p->file);

	p->reader = 0;
	p->io_pump = 0;
	
	p->current_audio_track = 0;
	
	p->block_buffer = 0;
	p->block_buffer_capacity = 0;
	
	int i;
	for(i=0; i<MKV_VIDEO_QUEUE_MAX; i++)
		zero_set_mkv_read_output_struct(&(p->video_queue[i]));
		
	p->video_queue_front = 0;
	p->video_queue_rear = 0;
	p->video_queue_size = 0;
		
	for(i=0; i<MKV_AUDIO_QUEUE_MAX; i++)
		zero_set_mkv_read_output_struct(&(p->audio_queue[i]));
	
	p->audio_queue_front = 0;
	p->audio_queue_rear = 0;
	p->audio_queue_size = 0;
	
	p->cluster_size = 0;
	p->cluster_timecode = 0;
	p->blockgroup_size = 0;
	p->block_size = 0;
	memset(&p->video_watermark, 0, sizeof(p->video_watermark));
	memset(&p->audio_watermark, 0, sizeof(p->audio_watermark));
	p->video_trim_floor = MKV_VIDEO_TRIM_FLOOR;
	p->audio_trim_floor = MKV_AUDIO_TRIM_FLOOR;

}

void mkv_read_close(struct mkv_read_struct *p) {
	int i;

	mkv_file_close(&p->file);

	if (p->reader)
		buffered_reader_close(p->reader);

	if (p->io_pump)
		ppa_io_pump_destroy(p->io_pump);

	mkv_release_block_buffer(p);

	for (i = 0; i < MKV_VIDEO_QUEUE_MAX; i++) {
		mkv_read_release_output(&p->video_queue[i]);
	}

	for (i = 0; i < MKV_AUDIO_QUEUE_MAX; i++) {
		mkv_read_release_output(&p->audio_queue[i]);
	}

	ppa_packet_pool_close(&g_mkv_video_packet_pool);
	ppa_packet_pool_close(&g_mkv_audio_packet_pool);

	mkv_read_safe_constructor(p);
}

char *mkv_read_open(struct mkv_read_struct *p, char *s)
{
	char *result;
	uint64_t initial_pos = 0;

	mkv_read_safe_constructor(p);

	result = mkv_file_open(&p->file, s);
	if (result != 0) {
		mkv_read_close(p);
		return result;
	}

	mkv_read_init_watermarks(p);

	if (!ppa_packet_pool_init(&g_mkv_video_packet_pool,
	                          MKV_VIDEO_QUEUE_MAX + 2,
	                          p->video_trim_floor)) {
		mkv_read_close(p);
		return "mkv_read_open: can not init video packet pool";
	}

	ppa_packet_pool_set_kind(&g_mkv_video_packet_pool,
	                               PPA_PACKET_POOL_KIND_VIDEO);

	if (!ppa_packet_pool_init(&g_mkv_audio_packet_pool,
	                          MKV_AUDIO_QUEUE_MAX + 2,
	                          p->audio_trim_floor)) {
		mkv_read_close(p);
		return "mkv_read_open: can not init audio packet pool";
	}

	ppa_packet_pool_set_kind(&g_mkv_audio_packet_pool,
	                               PPA_PACKET_POOL_KIND_AUDIO);

	if (p->file.info->total_indexes > 0) {
		initial_pos = p->file.info->indexes[0].filepos;
	}
	else if (p->file.info->has_first_cluster_pos) {
		initial_pos = p->file.info->first_cluster_pos;
	}
	else {
		mkv_read_close(p);
		return "mkv_read_open: no initial cluster position";
	}

	p->io_pump = ppa_io_pump_create();
	if (p->io_pump == 0) {
		mkv_read_close(p);
		return "mkv_read_open: can not create I/O pump";
	}
	p->reader = buffered_reader_open_with_pump_at(
		s, 96 * 1024, 0, 0x30, p->io_pump,
		MKV_IO_STREAM_INTERLEAVED, initial_pos);
	if (!p->reader) {
		mkv_read_close(p);
		return "mkv_read_open: can't open file";
	}

	mkv_read_refresh_watermarks(p);
	return 0;
}

int mkv_read_block_lacing(uint8_t *buffer,
                          uint64_t *size,
                          uint8_t *laces,
                          int32_t *all_lace_sizes)
{
	uint8_t flags;
	uint8_t lace_mode;
	uint32_t frame_count;
	uint64_t remaining;
	uint64_t total = 0;
	uint32_t i;

	if (buffer == 0 || size == 0 || laces == 0 || all_lace_sizes == 0)
		return 0;

	if (*size < 1)
		return 0;

	memset(all_lace_sizes, 0, sizeof(int32_t) * MKV_MAX_LACES);

	flags = *buffer++;
	remaining = *size - 1;
	lace_mode = (flags & 0x06) >> 1;

	if (lace_mode == 0) {
		if (remaining > INT_MAX)
			return 0;

		*laces = 1;
		all_lace_sizes[0] = (int32_t)remaining;
		*size = remaining;
		return 1;
	}

	if (remaining < 1)
		return 0;

	frame_count = ((uint32_t)(*buffer++)) + 1;
	remaining--;

	if (frame_count < 1 || frame_count > MKV_MAX_LACES)
		return 0;

	*laces = (uint8_t)frame_count;

	switch (lace_mode) {
		case 1: {
			for (i = 0; i < frame_count - 1; i++) {
				uint64_t one_size = 0;
				uint8_t byte;

				do {
					if (remaining < 1)
						return 0;

					byte = *buffer++;
					remaining--;

					one_size += byte;

					if (one_size > MKV_MAX_BLOCK_SIZE)
						return 0;
				} while (byte == 0xFF);

				if (one_size > INT_MAX)
					return 0;

				all_lace_sizes[i] = (int32_t)one_size;
				total += one_size;

				if (total > remaining)
					return 0;
			}

			if (remaining < total)
				return 0;

			if (remaining - total > INT_MAX)
				return 0;

			all_lace_sizes[frame_count - 1] = (int32_t)(remaining - total);
			*size = remaining;
			return 1;
		}

		case 2: {
			if (frame_count == 0)
				return 0;

			if ((remaining % frame_count) != 0)
				return 0;

			if ((remaining / frame_count) > INT_MAX)
				return 0;

			for (i = 0; i < frame_count; i++)
				all_lace_sizes[i] = (int32_t)(remaining / frame_count);

			*size = remaining;
			return 1;
		}

		case 3: {
			int32_t consumed = 0;
			uint64_t first_size;

			first_size =
				mkv_block_read_vint_uint_checked(buffer,
				                                 remaining,
				                                 &consumed);

			if (first_size == EBML_UINT_INVALID)
				return 0;

			if (first_size > INT_MAX)
				return 0;

			buffer += consumed;
			remaining -= consumed;

			all_lace_sizes[0] = (int32_t)first_size;
			total = first_size;

			for (i = 1; i < frame_count - 1; i++) {
				int64_t delta;
				int64_t current_size;

				delta =
					mkv_block_read_vint_int_checked(buffer,
					                                remaining,
					                                &consumed);

				if (delta == EBML_INT_INVALID)
					return 0;

				buffer += consumed;
				remaining -= consumed;

				current_size = (int64_t)all_lace_sizes[i - 1] + delta;

				if (current_size < 0 || current_size > INT_MAX)
					return 0;

				all_lace_sizes[i] = (int32_t)current_size;
				total += (uint64_t)current_size;

				if (total > remaining)
					return 0;
			}

			if (remaining < total)
				return 0;

			if (remaining - total > INT_MAX)
				return 0;

			all_lace_sizes[frame_count - 1] = (int32_t)(remaining - total);
			*size = remaining;
			return 1;
		}

		default:
			return 0;
	}
}

int mkv_is_subtitle_track(struct mkv_read_struct *p, int32_t tracknum, uint32_t* type) {
	int i;
	for(i=0; i<p->file.subtitle_tracks; i++) {
		if ( p->file.info->tracks[p->file.subtitle_track_ids[i]]->tracknum == tracknum ) {
			*type = p->file.info->tracks[p->file.subtitle_track_ids[i]]->video_type;
			return 1;
		}
	}
	return 0;
}

void mkv_handle_text_subtitle_block(struct mkv_read_struct *p,
                                    uint8_t *block_buffer,
                                    uint64_t block_size,
                                    uint64_t timecode,
                                    uint64_t duration,
                                    int32_t tracknum,
                                    uint32_t type)
{
	char trackname[32];
	struct subtitle_parse_struct *cur_parser = 0;
	struct subtitle_frame_struct *frame;
	unsigned int subtitle_flags = 0;
	mkvinfo_track_t *subtitle_track = 0;
	int i;

	if (p == 0 || block_buffer == 0 || block_size == 0)
		return;

	memset(trackname, 0, sizeof(trackname));
	snprintf(trackname, sizeof(trackname), "mkv subtitle track(%ld)", (long)tracknum);

	for (i = 0; i < MAX_SUBTITLES; i++) {
		if (strcmp(trackname, subtitle_parser[i].filename) == 0) {
			cur_parser = &subtitle_parser[i];
			break;
		}
	}

	if (cur_parser == 0)
		return;

	for (i = 0; i < p->file.subtitle_tracks; i++) {
		mkvinfo_track_t *track =
			p->file.info->tracks[p->file.subtitle_track_ids[i]];

		if (track != 0 && track->tracknum == tracknum) {
			subtitle_track = track;
			break;
		}
	}

	frame =
		(struct subtitle_frame_struct*)malloc_64(sizeof(struct subtitle_frame_struct));
	if (frame == 0)
		return;

	subtitle_frame_safe_constructor(frame);

	frame->p_start_frame =
		ppa_timestamp_ms_to_frame((unsigned)timecode,
		                          p->file.video_rate,
		                          p->file.video_scale);

	if (duration == 0)
		duration = 2000;

	frame->p_end_frame =
		ppa_timestamp_ms_to_frame((unsigned)(timecode + duration),
		                          p->file.video_rate,
		                          p->file.video_scale);

	if (frame->p_end_frame <= frame->p_start_frame)
		frame->p_end_frame = frame->p_start_frame + 1;

	if (!subtitle_text_normalize_mkv_payload(frame->p_string,
	                                         max_subtitle_string,
	                                         &frame->p_num_lines,
	                                         &subtitle_flags,
	                                         block_buffer,
	                                         block_size,
	                                         type,
	                                         subtitle_track ? subtitle_track->private_data : 0,
	                                         subtitle_track ? subtitle_track->private_size : 0)) {
		free_64(frame);
		return;
	}

	subtitle_parse_add_frame(cur_parser, cur_parser->p_cur_sub_frame, frame);
}

int mkv_handle_block(struct mkv_read_struct *p,
                     uint8_t *block_buffer,
                     uint64_t block_size,
                     uint64_t block_duration,
                     int64_t discard_padding_ns,
                     int64_t block_bref,
                     int64_t block_fref,
                     uint8_t simpleblock,
                     int track_id,
                     int skip_to_keyframe,
                     int seek_timestamp)
{
	int32_t tracknum;
	int32_t current_tracknum;
	int32_t video_tracknum;
	int32_t audio_tracknum;
	int32_t tmp = 0;
	int16_t time;
	uint8_t flags;
	uint64_t old_block_size;
	int32_t lace_size[MKV_MAX_LACES];
	uint8_t laces = 0;
	int64_t timecode;
	uint64_t duration;
	int res;
	int use_this_block = 1;

	if (p == 0 || block_buffer == 0)
		return 0;

	if (track_id < 0 || track_id >= p->file.info->total_tracks)
		return 0;

	if (block_size < 4)
		return 0;

	current_tracknum = p->file.info->tracks[track_id]->tracknum;
	video_tracknum = p->file.info->tracks[p->file.video_track_id]->tracknum;
	audio_tracknum =
		p->file.info->tracks[p->file.audio_track_ids[p->current_audio_track]]->tracknum;

	tracknum = (int32_t)mkv_block_read_vint_uint_checked(block_buffer,
	                                                     block_size,
	                                                     &tmp);

	if (tracknum == (int32_t)EBML_UINT_INVALID)
		return 0;

	if (tmp <= 0 || (uint64_t)tmp + 3 > block_size)
		return 0;

	/* Cluster bytes still pass through the bounded reader, but no video
	 * packet, lacing workspace or AVC job survives this admission boundary. */
	if (ppa_session_audio_only() && tracknum != audio_tracknum)
		return 0;

	block_buffer += tmp;
	block_size -= tmp;

	time = (int16_t)(((uint16_t)block_buffer[0] << 8) |
	                 ((uint16_t)block_buffer[1]));

	block_buffer += 2;
	block_size -= 2;

	old_block_size = block_size;
	flags = block_buffer[0];

	res = mkv_read_block_lacing(block_buffer, &block_size, &laces, lace_size);
	if (res != 1)
		return res;

	if (old_block_size < block_size)
		return 0;

	block_buffer += old_block_size - block_size;

	/* Both unsigned EBML timestamps must fit before taking their signed
	 * difference. Add the signed block offset only after guarding its edge. */
	if (p->cluster_timecode > INT64_MAX ||
	    p->file.info->first_timecode > INT64_MAX)
		return 0;
	timecode = (int64_t)p->cluster_timecode -
	           (int64_t)p->file.info->first_timecode;
	if ((time > 0 && timecode > INT64_MAX - time) ||
	    (time < 0 && timecode < INT64_MIN - time))
		return 0;
	timecode = ppa_mkv_timecode_to_ms(timecode + time,
	                                  p->file.info->timecode_scale);

	if (timecode < 0)
		timecode = 0;
	if (timecode > INT_MAX || block_duration > INT64_MAX)
		return 0;

	if (block_duration != 0) {
		duration = (uint64_t)ppa_mkv_timecode_to_ms((int64_t)block_duration,
		                                            p->file.info->timecode_scale);
	}
	else if (tracknum == video_tracknum &&
	         p->file.video_rate != 0) {
		duration = (1000ULL * (uint64_t)p->file.video_scale) /
		           (uint64_t)p->file.video_rate;
	}
	else if (tracknum == audio_tracknum &&
	         p->file.audio_rate != 0) {
		duration = (1000ULL * (uint64_t)p->file.audio_resample_scale) /
		           (uint64_t)p->file.audio_rate;
	}
	else {
		duration = 0;
	}
	if (duration > INT_MAX - (uint64_t)timecode)
		return 0;

	res = 1;

	{
		uint32_t subtitle_type;

		if (mkv_is_subtitle_track(p, tracknum, &subtitle_type)) {
			uint64_t subtitle_duration = duration;

			if (subtitle_duration == 0)
				subtitle_duration = 2000;

			mkv_handle_text_subtitle_block(p,
			                               block_buffer,
			                               block_size,
			                               timecode,
			                               subtitle_duration,
			                               tracknum,
			                               subtitle_type);

			use_this_block = 0;
			res = 0;
		}
		else if (tracknum == current_tracknum) {
			if (skip_to_keyframe) {
				if (simpleblock) {
					if (!(flags & 0x80)) {
						use_this_block = 0;
						res = 0;
					}
				}
				else if (block_bref != 0 || block_fref != 0) {
					use_this_block = 0;
					res = 0;
				}
				else if (seek_timestamp > 0 && timecode < seek_timestamp) {
					use_this_block = 0;
					res = 0;
				}
			}
		}
		else {
			use_this_block = 0;
			res = 0;

			if (!skip_to_keyframe &&
			    (tracknum == video_tracknum || tracknum == audio_tracknum)) {
				use_this_block = 1;
			}
		}
	}

	if (use_this_block) {
		mkvinfo_track_t *current_track = 0;
		int i;
        uint32_t audio_lace_frames[MKV_MAX_LACES];
        uint64_t audio_frames_before = 0, audio_frames_total = 0;
        uint64_t trim_first = 0, trim_after = 0;
        const struct ppu_audio_format *audio_format = &p->file.audio_formats[p->current_audio_track];


		if (tracknum == video_tracknum)
			current_track = p->file.info->tracks[p->file.video_track_id];
		else if (tracknum == audio_tracknum)
			current_track = p->file.info->tracks[p->file.audio_track_ids[p->current_audio_track]];
		else
			return 0;

        if (tracknum == audio_tracknum) {
            const uint8_t *lace_data = block_buffer;
            uint64_t left = block_size;
            for (i = 0; i < laces; ++i) {
                int frames;
                if (lace_size[i] <= 0 || (uint64_t)lace_size[i] > left) return -3;
                /* Header stripping is restored when the packet is queued.
                 * The admitted MP3 rates already determine MPEG-1/2 duration;
                 * validate the reconstructed header again before PCM use. */
                if (audio_format->type == PPU_AUDIO_MP3 && current_track->compress_setting_size)
                    frames = audio_format->rate < 32000 ? 576 : 1152;
                else
                    frames = ppu_audio_packet_frames(audio_format->type, lace_data,
                                                      (unsigned int)lace_size[i], audio_format->rate);
                if (frames <= 0 || (unsigned int)frames > audio_format->max_frames) return -3;
                audio_lace_frames[i] = (unsigned int)frames;
                audio_frames_total += (unsigned int)frames;
                lace_data += lace_size[i]; left -= lace_size[i];
            }
            trim_after = audio_frames_total;
            if (discard_padding_ns) {
                uint64_t ns = discard_padding_ns < 0 ? (uint64_t)-discard_padding_ns : (uint64_t)discard_padding_ns;
                uint64_t count = (ns * audio_format->rate + 500000000ULL) / 1000000000ULL;
                if (count > audio_frames_total) return -3;
                if (discard_padding_ns < 0) trim_first = count;
                else trim_after -= count;
            }
        }

		for (i = 0; i < laces; i++) {
			struct mkv_read_output_struct packet;
			struct ppa_packet_pool *pool;
			struct ppa_packet_slot *slot;
			unsigned int payload_size;
			unsigned int header_size;

			if (lace_size[i] < 0)
				return 0;

			payload_size = (unsigned int)lace_size[i];
			header_size = current_track->compress_setting_size;

			if (payload_size > MKV_MAX_BLOCK_SIZE)
				return 0;

			if (header_size > 16 * 1024)
				return 0;

			if (payload_size + header_size < payload_size)
				return 0;

			if ((uint64_t)payload_size > block_size)
				return 0;

			memset(&packet, 0, sizeof(struct mkv_read_output_struct));

			packet.size = payload_size + header_size;
            if (tracknum == audio_tracknum && packet.size > PPU_AUDIO_MAX_PACKET_BYTES)
                return -3;

			if (tracknum == video_tracknum)
				pool = &g_mkv_video_packet_pool;
			else
				pool = &g_mkv_audio_packet_pool;

			slot = ppa_packet_pool_acquire(pool, packet.size);
			if (slot == 0)
				return -2;

			packet.data = slot->data;
			packet.pool = pool;
			packet.slot = slot;
			/* Derive every lace from its original block anchor. Repeatedly
			 * adding floor(frame_ms) drifts across AAC/MP3 and fractional FPS. */
			{
				uint64_t step = 0;
				if (tracknum == video_tracknum && p->file.video_rate != 0)
					step = 1000ULL * (unsigned int)i * p->file.video_scale /
					       p->file.video_rate;
				else if (tracknum == audio_tracknum)
                    step = 1000ULL * audio_frames_before / audio_format->rate;
				if (step > INT_MAX - (uint64_t)timecode) {
					mkv_read_release_output(&packet);
					return 0;
				}
				packet.timestamp = (int)(timecode + step);
			}

            if (tracknum == audio_tracknum) {
                uint64_t end = audio_frames_before + audio_lace_frames[i];
                if (audio_frames_before < trim_first) {
                    uint64_t count = trim_first - audio_frames_before;
                    packet.audio_trim_start = count > audio_lace_frames[i] ? audio_lace_frames[i] : (uint32_t)count;
                }
                if (end > trim_after) {
                    uint64_t count = end - trim_after;
                    packet.audio_trim_end = count > audio_lace_frames[i] ? audio_lace_frames[i] : (uint32_t)count;
                }
                audio_frames_before = end;
            }

			slot->size = packet.size;
			slot->timestamp = (unsigned int)packet.timestamp;
			slot->state = PPA_PACKET_FILLING;

			if (header_size > 0) {
				memcpy((uint8_t *)packet.data,
				       current_track->compress_setting,
				       header_size);
			}

			memcpy((uint8_t *)packet.data + header_size,
			       block_buffer,
			       payload_size);

			if (tracknum == video_tracknum) {
				tmp = in_mkv_read_queue(p->video_queue,
				                        &p->video_queue_size,
				                        &p->video_queue_rear,
				                        MKV_VIDEO_QUEUE_MAX,
				                        &packet);
			}
			else {
				tmp = in_mkv_read_queue(p->audio_queue,
				                        &p->audio_queue_size,
				                        &p->audio_queue_rear,
				                        MKV_AUDIO_QUEUE_MAX,
				                        &packet);
			}

			if (tmp == 0) {
				mkv_read_release_output(&packet);
				return -1;
			}
			mkv_read_refresh_watermarks(p);

			block_buffer += payload_size;
			block_size -= payload_size;

		}
	}

	return res;
}

char *mkv_read_fill_buffer(struct mkv_read_struct *p,
                           int track_id,
                           int skip_to_keyframe,
                           int seek_timestamp)
{
	int32_t il;
	int32_t tmp;
	uint64_t l;
	int res;
	unsigned int recovery_budget = 2U;
	char *recovery_reason = 0;

	while (1) {
		if (!skip_to_keyframe) {
			if (track_id == p->file.video_track_id &&
			    ppa_queue_watermark_target_reached(&p->audio_watermark))
				return MKV_READ_VIDEO_AUDIO_BACKPRESSURE;
			if (!ppa_session_audio_only() && p->file.audio_tracks > 0 &&
			    track_id == p->file.audio_track_ids[p->current_audio_track] &&
			    ppa_queue_watermark_target_reached(&p->video_watermark))
				return MKV_READ_AUDIO_VIDEO_BACKPRESSURE;
		}
		while (p->cluster_size > 0) {
			uint64_t block_duration = 0;
			int64_t discard_padding_ns = 0;
			int64_t block_bref = 0;
			int64_t block_fref = 0;
			int have_block = 0;

			if (!skip_to_keyframe) {
				if (track_id == p->file.video_track_id &&
				    ppa_queue_watermark_target_reached(&p->audio_watermark))
					return MKV_READ_VIDEO_AUDIO_BACKPRESSURE;
				if (!ppa_session_audio_only() && p->file.audio_tracks > 0 &&
				    track_id == p->file.audio_track_ids[p->current_audio_track] &&
				    ppa_queue_watermark_target_reached(&p->video_watermark))
					return MKV_READ_AUDIO_VIDEO_BACKPRESSURE;
			}

			while (p->blockgroup_size > 0) {
				uint32_t id = mkv_ebml_read_id(p->reader, &il);

				switch (id) {
					case MATROSKA_ID_BLOCKDURATION:
						block_duration = mkv_ebml_read_uint(p->reader, &l);
						if (block_duration == EBML_UINT_INVALID) {
							recovery_reason = "error block duration";
							goto recover_cluster;
						}
						break;

					case MATROSKA_ID_BLOCK: {
						char *buffer_result;

						p->block_size = mkv_ebml_read_length(p->reader, &tmp);

						if (p->block_size == EBML_UINT_INVALID ||
						    p->block_size > MKV_MAX_BLOCK_SIZE ||
						    p->block_size > SIZE_MAX - MKV_INPUT_PADDING) {
							recovery_reason = "error block size";
							goto recover_cluster;
						}

						buffer_result =
							mkv_ensure_block_buffer(p,
							                        p->block_size + MKV_INPUT_PADDING);
						if (buffer_result != 0)
							return buffer_result;

						if (!buffered_reader_read_exact(p->reader,
						                                p->block_buffer,
						                                (uint32_t)p->block_size)) {
							recovery_reason = "can not read block";
							goto recover_cluster;
						}

						memset(((uint8_t *)p->block_buffer) + p->block_size,
						       0,
						       MKV_INPUT_PADDING);

						l = tmp + p->block_size;
						have_block = 1;
						break;
					}

					case MATROSKA_ID_REFERENCEBLOCK: {
						int64_t num = mkv_ebml_read_int(p->reader, &l);

						if (num == EBML_INT_INVALID) {
							recovery_reason = "can not find block ref";
							goto recover_cluster;
						}

						if (num <= 0)
							block_bref = num;
						else
							block_fref = num;

						break;
					}

                    case MATROSKA_ID_DISCARDPADDING:
                        discard_padding_ns = mkv_ebml_read_int(p->reader, &l);
                        if (discard_padding_ns == EBML_INT_INVALID ||
                            discard_padding_ns < -10000000000LL || discard_padding_ns > 10000000000LL) {
                            recovery_reason = "invalid audio discard padding";
                            goto recover_cluster;
                        }
                        break;
					case MATROSKA_ID_BLOCKADDITIONS:
					case EBML_ID_CRC32:
					case EBML_ID_VOID:
					default:
						if (id == EBML_ID_INVALID) {
							recovery_reason = "read id invalid";
							goto recover_cluster;
						}

						if (mkv_ebml_read_skip(p->reader, &l)) {
							recovery_reason = "skip failed";
							goto recover_cluster;
						}
						break;
				}

				if (!mkv_master_subtract(&p->blockgroup_size, l + il)) {
					recovery_reason = "blockgroup size underflow";
					goto recover_cluster;
				}

				if (!mkv_master_subtract(&p->cluster_size, l + il)) {
					recovery_reason = "cluster size underflow";
					goto recover_cluster;
				}
			}

			if (have_block) {
				res = mkv_handle_block(p,
				                       (uint8_t *)(p->block_buffer),
				                       p->block_size,
				                       block_duration,
				                       discard_padding_ns,
				                       block_bref,
				                       block_fref,
				                       0,
				                       track_id,
				                       skip_to_keyframe,
				                       seek_timestamp);

				if (res < 0) {
					if (res == -1)
						return "mkv_read_fill_buffer: queue is full";
					else
						return "mkv_read_fill_buffer: can not acquire packet buffer";
				}

				if (res)
					return 0;
			}

			if (p->cluster_size > 0) {
				uint32_t id = mkv_ebml_read_id(p->reader, &il);

				switch (id) {
					case MATROSKA_ID_CLUSTERTIMECODE: {
						uint64_t num = mkv_ebml_read_uint(p->reader, &l);

						if (num == EBML_UINT_INVALID || num > INT64_MAX) {
							recovery_reason = "invalid cluster timecode";
							goto recover_cluster;
						}

						p->cluster_timecode = num;
						break;
					}

					case MATROSKA_ID_BLOCKGROUP:
						p->blockgroup_size = mkv_ebml_read_length(p->reader, &tmp);
						if (p->blockgroup_size == EBML_UINT_INVALID) {
							recovery_reason = "unsupported unknown-size blockgroup";
							goto recover_cluster;
						}

						l = tmp;
						break;

					case MATROSKA_ID_SIMPLEBLOCK: {
						struct PpaBufferedReaderSpan borrowed;
						char *buffer_result;
						uint8_t *block_data;

						memset(&borrowed, 0, sizeof(borrowed));
						p->block_size = mkv_ebml_read_length(p->reader, &tmp);

						if (p->block_size == EBML_UINT_INVALID ||
						    p->block_size > MKV_MAX_BLOCK_SIZE ||
						    p->block_size > SIZE_MAX - MKV_INPUT_PADDING) {
							recovery_reason = "error block size";
							goto recover_cluster;
						}

						/*
						 * The borrowed view is consumed synchronously by EBML lacing
						 * and packet creation below, then released before any return.
						 * Queued packets still receive their established owned,
						 * padded pool copy; no reader-window pointer reaches Sony's
						 * decoder or survives a seek generation.
						 */
						if (buffered_reader_borrow_current(
						        p->reader, (uint32_t)p->block_size,
						        1U, 0U, &borrowed)) {
							block_data = (uint8_t *)borrowed.data;
						}
						else {
							buffer_result =
								mkv_ensure_block_buffer(
								    p,
								    p->block_size + MKV_INPUT_PADDING);
							if (buffer_result != 0)
								return buffer_result;

							if (!buffered_reader_read_exact(
							        p->reader, p->block_buffer,
							        (uint32_t)p->block_size)) {
								recovery_reason = "can not read block";
								goto recover_cluster;
							}

							memset(
							    ((uint8_t *)p->block_buffer) +
							        p->block_size,
							    0, MKV_INPUT_PADDING);
							block_data = (uint8_t *)p->block_buffer;
						}

						l = tmp + p->block_size;
						res = mkv_handle_block(p,
						                       block_data,
						                       p->block_size,
						                       block_duration,
						                       discard_padding_ns,
						                       block_bref,
						                       block_fref,
						                       1,
						                       track_id,
						                       skip_to_keyframe,
						                       seek_timestamp);
						buffered_reader_release_span(&borrowed);

						if (res < 0) {
							if (res == -1)
								return "mkv_read_fill_buffer: queue is full";
							else
								return "mkv_read_fill_buffer: can not acquire packet buffer";
						}

						if (res) {
							if (!mkv_master_subtract(&p->cluster_size, l + il)) {
								recovery_reason = "cluster size underflow";
								goto recover_cluster;
							}

							return 0;
						}

						break;
					}

					case EBML_ID_CRC32:
					case EBML_ID_VOID:
					default:
						if (id == EBML_ID_INVALID) {
							recovery_reason = "can not find block";
							goto recover_cluster;
						}

						if (mkv_ebml_read_skip(p->reader, &l)) {
							recovery_reason = "skip failed";
							goto recover_cluster;
						}
						break;
				}

				if (!mkv_master_subtract(&p->cluster_size, l + il)) {
					recovery_reason = "cluster size underflow";
					goto recover_cluster;
				}
			}
		}

        {
            int64_t position = buffered_reader_position64(p->reader);
            int64_t length = buffered_reader_length64(p->reader);
            uint32_t id;
            if (position >= 0 && length >= 0 && position == length) return MKV_READ_EOF;
            id = mkv_ebml_read_id(p->reader, &il);
            if (id == MATROSKA_ID_CUES || id == MATROSKA_ID_SEEKHEAD ||
                id == EBML_ID_VOID || id == EBML_ID_CRC32 ||
                id == 0x1254C367U || id == 0x1043A770U || id == 0x1941A469U) {
                if (mkv_ebml_read_skip(p->reader, &l)) return "mkv_read_fill_buffer: invalid trailing metadata";
                continue;
            }
			if (id != MATROSKA_ID_CLUSTER) {
				recovery_reason = "cluster id not found";
				goto recover_cluster;
			}

			p->cluster_size = mkv_ebml_read_length(p->reader, 0);

			if (p->cluster_size == EBML_UINT_INVALID) {
				recovery_reason = "unsupported unknown-size cluster";
				goto recover_cluster;
			}
		}
		continue;

recover_cluster:
		p->cluster_size = 0;
		p->blockgroup_size = 0;
		p->block_size = 0;
		if (recovery_budget > 0U) {
			recovery_budget--;
			if (mkv_read_resync_cluster(p, MKV_RECOVERY_SCAN_LIMIT,
			                            recovery_reason))
				continue;
		}
		return recovery_reason ? recovery_reason :
		       "mkv_read_fill_buffer: unrecoverable container damage";
	}

	return 0;
}

char *mkv_read_seek(struct mkv_read_struct *p, int timestamp, int last_timestamp)
{
	int32_t video_tracknum;
	int32_t i;
	mkvinfo_index_t *index = 0;
	int64_t best_time = -1;

	if (p == 0 || p->file.info == 0)
		return "mkv_read_seek: invalid reader";
	(void)last_timestamp;

	if (timestamp < 0)
		timestamp = 0;

    {
        const struct ppu_audio_format *f = &p->file.audio_formats[p->current_audio_track];
        unsigned int preroll_ms = f->preroll_frames ?
            (unsigned int)((uint64_t)f->preroll_frames * 1000ULL / f->rate) + 120U : 0;
        timestamp = (unsigned int)timestamp > preroll_ms ? timestamp - (int)preroll_ms : 0;
    }

	video_tracknum = p->file.info->tracks[p->file.video_track_id]->tracknum;

	if (p->file.info->total_indexes <= 0) {
		if (timestamp == 0 && p->file.info->has_first_cluster_pos) {
			if (mkv_read_seek_abs64(p->reader,
			                        p->file.info->first_cluster_pos) < 0)
				return "mkv_read_seek: file seek failed";

			p->cluster_size = 0;
			p->blockgroup_size = 0;

			mkv_read_clear_queues_at_safe_point(p);
		}

		return 0;
	}

	for (i = 0; i < p->file.info->total_indexes; i++) {
		int64_t timecode;

		if (p->file.info->indexes[i].tracknum != video_tracknum)
			continue;
		if (p->file.info->indexes[i].timecode > INT64_MAX ||
		    p->file.info->first_timecode > INT64_MAX)
			continue;

		timecode =
			ppa_mkv_timecode_to_ms((int64_t)p->file.info->indexes[i].timecode -
			                        (int64_t)p->file.info->first_timecode,
			                        p->file.info->timecode_scale);

		if (timecode <= timestamp && timecode >= best_time) {
			best_time = timecode;
			index = p->file.info->indexes + i;
		}
	}

	if (index == 0) {
		for (i = 0; i < p->file.info->total_indexes; i++) {
			if (p->file.info->indexes[i].tracknum == video_tracknum) {
				index = p->file.info->indexes + i;
				break;
			}
		}
	}

	if (index == 0)
		return 0;

	{
		/* H.264 must decode every predictive packet from the preceding cue/IDR.
		 * Filtering compressed packets here breaks the reference chain and made
		 * reverse seeks stall. The show thread suppresses presentation until the
		 * requested media timestamp while the decoder receives complete preroll. */
		int seek_timestamp = 0;
		char *result = 0;

		if (mkv_read_seek_abs64(p->reader, index->filepos) < 0)
			return "mkv_read_seek: file seek failed";

		p->cluster_size = 0;
		p->blockgroup_size = 0;

		mkv_read_clear_queues_at_safe_point(p);

		while (1) {
			result = mkv_read_fill_buffer(p,
			                              p->file.video_track_id,
			                              1,
			                              seek_timestamp);

			if (result)
				return result;

			if (p->video_queue_size > 0)
				break;
		}
	}

	return 0;
}

char *mkv_read_get_video(struct mkv_read_struct *p,
                         struct mkv_read_output_struct *output)
{
	if (p == 0 || output == 0)
		return "mkv_read_get_video: invalid argument";

	if (p->video_queue_size == 0U) {
		char *res;

		if (!ppa_session_audio_only() && p->file.audio_tracks > 0 &&
		    ppa_queue_watermark_target_reached(&p->audio_watermark))
			return MKV_READ_VIDEO_AUDIO_BACKPRESSURE;
		res = mkv_read_fill_buffer(p,
		                           p->file.video_track_id,
		                           0,
		                           0);
		if (res)
			return res;
		if (p->video_queue_size == 0U)
			return "mkv_read_get_video: video queue is empty";
	}

	if (!out_mkv_read_queue(p->video_queue,
	                        &p->video_queue_size,
	                        &p->video_queue_front,
	                        MKV_VIDEO_QUEUE_MAX,
	                        output))
		return "mkv_read_get_video: video queue pop failed";
	mkv_read_refresh_watermarks(p);
	return 0;
}

char *mkv_read_get_audio(struct mkv_read_struct *p,
                         unsigned int audio_stream,
                         struct mkv_read_output_struct *output)
{
	if (p == 0 || output == 0 || audio_stream >= (unsigned int)p->file.audio_tracks)
		return "mkv_read_get_audio: invalid audio stream";

	if (p->current_audio_track != (int)audio_stream) {
		clear_mkv_read_queue(p->audio_queue,
		                     &p->audio_queue_size,
		                     &p->audio_queue_front,
		                     &p->audio_queue_rear,
		                     MKV_AUDIO_QUEUE_MAX);
		p->current_audio_track = (int)audio_stream;
		ppa_queue_watermark_reset(&p->audio_watermark);
		ppa_packet_pool_trim_free(&g_mkv_audio_packet_pool,
		                          p->audio_trim_floor, 0);
		mkv_release_block_buffer(p);
		p->block_size = 0;
		mkv_read_refresh_watermarks(p);
	}

	if (p->audio_queue_size == 0U) {
		char *res;

		if (!ppa_session_audio_only() && ppa_queue_watermark_target_reached(&p->video_watermark))
			return MKV_READ_AUDIO_VIDEO_BACKPRESSURE;
		res = mkv_read_fill_buffer(p,
		                           p->file.audio_track_ids[audio_stream],
		                           0,
		                           0);
		if (res)
			return res;
		if (p->audio_queue_size == 0U)
			return "mkv_read_get_audio: audio queue is empty";
	}

	if (!out_mkv_read_queue(p->audio_queue,
	                        &p->audio_queue_size,
	                        &p->audio_queue_front,
	                        MKV_AUDIO_QUEUE_MAX,
	                        output))
		return "mkv_read_get_audio: audio queue pop failed";
	mkv_read_refresh_watermarks(p);
	return 0;
}
