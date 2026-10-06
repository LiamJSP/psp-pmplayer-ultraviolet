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

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include "bufferedio.h"
#include "mp4info_type.h"

#define ATOM_TYPE(a,b,c,d) \
	( ((uint32_t)d) | (((uint32_t)c) << 8) | (((uint32_t)b) << 16) | (((uint32_t)a) << 24) )


static const uint32_t aac_samplerates[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000};

#define MP4INFO_MAX_TABLE_ENTRIES 4000000U
#define MP4INFO_MAX_STTS_ENTRIES  262144U
#define MP4INFO_MAX_CODEC_CONFIG  (64U * 1024U)

static uint32_t atom_get_size(uint8_t *data) {
	uint32_t result;
	uint32_t a, b, c, d;

	a = (uint8_t)data[0];
	b = (uint8_t)data[1];
	c = (uint8_t)data[2];
	d = (uint8_t)data[3];
	result = (a << 24) | (b << 16) | (c << 8) | d;

	return result;
}

static uint32_t atom_get_type(uint8_t *data) {
	uint32_t result;
	uint32_t a, b, c, d;

	a = (uint8_t)data[0];
	b = (uint8_t)data[1];
	c = (uint8_t)data[2];
	d = (uint8_t)data[3];
	result = (a << 24) | (b << 16) | (c << 8) | d;

	return result;
}

/* Every position belongs to one signed-64 native/VFS range. Subtract
 * before adding: even an extended-size atom cannot wrap an end position. */
static int atom_range_end(void *handle, int64_t start, uint64_t bytes,
                           int64_t *end)
{
    int64_t length = io_get_length64(handle);
    if (!handle || !end || start < 0 || length < start ||
        bytes > (uint64_t)(length - start))
        return 0;
    *end = start + (int64_t)bytes;
    return 1;
}

static uint64_t atom_read_header(void *handle, uint32_t *atom_type,
                                 uint32_t *header_size, uint64_t remaining)
{
    uint8_t header[8];
    uint64_t size;
    uint32_t small_size;
    int64_t start = io_get_position64(handle), end;
    /* Failed reads must yield size < header_size in every child walker,
     * including the first child. Otherwise a zero-size failure never advances. */
    if (atom_type) *atom_type = 0;
    if (header_size) *header_size = 8;
    if (!handle || !atom_type || !header_size || remaining < 8U ||
        !atom_range_end(handle, start, remaining, &end) ||
        io_read_data(handle, header, sizeof(header)) != sizeof(header))
        return 0;
    small_size = atom_get_size(header);
    size = small_size;
    *atom_type = atom_get_type(header + 4);
    if (small_size == 1U) {
        uint8_t extended[8];
        unsigned int i;
        if (remaining < 16U || io_read_data(handle, extended, 8) != 8)
            return 0;
        *header_size = 16;
        size = 0;
        for (i = 0; i < 8; ++i) size = (size << 8) | extended[i];
    } else if (small_size == 0U) {
        /* A size-zero atom extends to EOF, including when nested. The
         * remaining parent range below rejects one that escapes its parent. */
        size = (uint64_t)(io_get_length64(handle) - start);
    }
    if (size < *header_size || size > remaining ||
        !atom_range_end(handle, start, size, &end))
        return 0;
    return size;
}

/* A damaged leaf cannot leave its siblings parsing from inside its payload. */
static void atom_finish_child(void *handle, int64_t start, uint64_t bytes)
{
    int64_t end;
    if (atom_range_end(handle, start, bytes, &end) &&
        io_get_position64(handle) != end)
        (void)io_set_position64(handle, end);
}

static void parse_unused_atom(mp4info_t *info, const uint64_t total_size)
{
    if (info && info->handle)
        atom_finish_child(info->handle, io_get_position64(info->handle), total_size);
}

static int atom_payload_end(mp4info_t *info, uint64_t total_size, int64_t *end)
{
    return info && info->handle && atom_range_end(info->handle,
                         io_get_position64(info->handle), total_size, end);
}

static int atom_has_bytes(mp4info_t *info, int64_t end, uint32_t bytes)
{
    int64_t pos = io_get_position64(info->handle);
    return pos >= 0 && pos <= end && (uint64_t)bytes <= (uint64_t)(end - pos);
}

static void *atom_calloc_array(mp4info_t *info, uint32_t count, size_t element_size,
                               uint32_t hard_limit)
{
    size_t bytes;
    void *data;
    if (!info || info->parse_error || count == 0) return 0;
    if (count > hard_limit || element_size == 0 ||
        count > SIZE_MAX / element_size) {
        info->parse_error = 1;
        return 0;
    }
    bytes = (size_t)count * element_size;
    /* Widening co64 must not turn a hostile table into a desktop-sized heap.
     * This is cumulative admission, not a claim about measured live memory. */
    if (bytes > MP4INFO_MAX_METADATA_BYTES - info->metadata_allocated_bytes) {
        info->parse_error = 1;
        return 0;
    }
    data = calloc((size_t)count, element_size);
    if (!data) { info->parse_error = 1; return 0; }
    info->metadata_allocated_bytes += (uint32_t)bytes;
    return data;
}

static int read_mp4_descr_length_bounded(mp4info_t *info,
                                         int64_t end_position,
                                         uint32_t *length)
{
	uint8_t b;
	uint8_t num_bytes = 0;
	uint32_t value = 0;

	if (length == 0)
		return 0;
	do {
		if (io_get_position64(info->handle) >= end_position)
			return 0;
		b = io_read_8(info->handle);
		num_bytes++;
		value = (value << 7) | (b & 0x7f);
	} while ((b & 0x80) != 0 && num_bytes < 4);
	if ((b & 0x80) != 0)
		return 0;
	*length = value;
	return 1;
}

static int mp4_descr_has_bytes(mp4info_t *info, int64_t end_position,
                               uint32_t bytes)
{
	int64_t position = io_get_position64(info->handle);
	return position >= 0 && position <= end_position &&
	       (uint64_t)bytes <= (uint64_t)(end_position - position);
}

static void read_esds_atom(mp4info_t* info, const uint64_t total_size) {
	int64_t start_position = io_get_position64(info->handle);
	int64_t dest_position;
	int64_t decoder_end;
	mp4info_track_t *track = info->tracks[info->total_tracks - 1];
	uint8_t tag;
	uint8_t object_type;
	uint32_t length;

	if (!atom_range_end(info->handle, start_position, total_size, &dest_position))
		return;
	if (!mp4_descr_has_bytes(info, dest_position, 4))
		goto done;
	io_read_8(info->handle); /* version */
	io_read_be24(info->handle); /* flags */
	if (!mp4_descr_has_bytes(info, dest_position, 1))
		goto done;

	tag = io_read_8(info->handle);
	if (tag == 0x03) {
		uint8_t flags;
		int64_t es_end;
		if (!read_mp4_descr_length_bounded(info, dest_position, &length) ||
		    length < 3 || !mp4_descr_has_bytes(info, dest_position, length))
			goto done;
		es_end = io_get_position64(info->handle) + (int64_t)length;
		io_read_be16(info->handle); /* ES_ID */
		flags = io_read_8(info->handle);
		if ((flags & 0x80) != 0) {
			if (!mp4_descr_has_bytes(info, es_end, 2)) goto done;
			io_read_be16(info->handle); /* dependsOn_ES_ID */
		}
		if ((flags & 0x40) != 0) {
			uint8_t url_length;
			if (!mp4_descr_has_bytes(info, es_end, 1)) goto done;
			url_length = io_read_8(info->handle);
			if (!mp4_descr_has_bytes(info, es_end, url_length)) goto done;
			io_set_position64(info->handle,
			                io_get_position64(info->handle) + url_length);
		}
		if ((flags & 0x20) != 0) {
			if (!mp4_descr_has_bytes(info, es_end, 2)) goto done;
			io_read_be16(info->handle); /* OCR_ES_ID */
		}
		if (!mp4_descr_has_bytes(info, es_end, 1))
			goto done;
		tag = io_read_8(info->handle);
	}
	else if (tag != 0x04) {
		/* Preserve compatibility with old files that omit the ES descriptor
		 * tag and place a three-byte ES header before DecoderConfig. */
		if (!mp4_descr_has_bytes(info, dest_position, 3))
			goto done;
		io_read_be16(info->handle);
		tag = io_read_8(info->handle);
	}

	if (tag != 0x04 ||
	    !read_mp4_descr_length_bounded(info, dest_position, &length) ||
	    length < 13 || !mp4_descr_has_bytes(info, dest_position, length))
		goto done;
	decoder_end = io_get_position64(info->handle) + (int64_t)length;

	object_type = io_read_8(info->handle);
	track->audio_object_type = object_type;
	io_read_be32(info->handle); /* streamType/bufferSizeDB */
	io_read_be32(info->handle); /* maxBitrate */
	io_read_be32(info->handle); /* avgBitrate */

	/* ISO/IEC 14496-1 objectTypeIndication values used for MPEG audio. */
	if (object_type == 0x69 || object_type == 0x6b) {
		track->audio_type = ATOM_TYPE('m','p','3',' ');
		goto done;
	}

	if (!mp4_descr_has_bytes(info, decoder_end, 1) ||
	    io_read_8(info->handle) != 0x05 ||
	    !read_mp4_descr_length_bounded(info, decoder_end, &length) ||
	    !mp4_descr_has_bytes(info, decoder_end, length))
		goto done;

	if (track->audio_type == ATOM_TYPE('m','p','4','a') && length >= 2) {
		uint8_t asc[4] = {0,0,0,0};
		uint32_t to_read = length < sizeof(asc) ? length : sizeof(asc);
		uint32_t sample_index;
		uint32_t parsed_samplerate = 0;
		uint32_t i;

		for (i = 0; i < to_read; ++i)
			asc[i] = io_read_8(info->handle);

		sample_index = ((uint32_t)(asc[0] & 0x07) << 1) |
		               ((uint32_t)asc[1] >> 7);
		if (sample_index < sizeof(aac_samplerates) / sizeof(aac_samplerates[0]))
			parsed_samplerate = aac_samplerates[sample_index];

		if (parsed_samplerate != 0) {
			track->mp4a_sbr_samplerate = track->samplerate;
			if (parsed_samplerate != track->samplerate) {
				track->samplerate = parsed_samplerate;
				track->mp4a_is_sbr = 1;
			}
		}

		/* AudioSpecificConfig channelConfiguration is authoritative when the
		 * sample entry reports zero, a pattern seen in some remuxers. */
		if (track->channels == 0)
			track->channels = (asc[1] >> 3) & 0x0f;
	}

done:
	io_set_position64(info->handle, dest_position);
}

/* Store only the bounded decoder configuration; skip optional FLAC metadata.
 * Every read is inside the child box. Duplicate configuration is rejected. */
static void read_software_audio_config(mp4info_t *info, uint32_t type, uint64_t size)
{
    int64_t end;
    mp4info_track_t *t = info->tracks[info->total_tracks - 1];
    if (!atom_payload_end(info, size, &end)) return;
    if (t->audio_private_size) { info->parse_error = 1; goto done; }
    if (type == ATOM_TYPE('d','O','p','s') &&
        t->audio_type == ATOM_TYPE('O','p','u','s') && size == 11) {
        if (io_read_data(info->handle, t->audio_private, 11) == 11)
            t->audio_private_size = 11;
    } else if (type == ATOM_TYPE('d','f','L','a') &&
               t->audio_type == ATOM_TYPE('f','L','a','C') && size >= 42) {
        if (io_read_be32(info->handle) != 0) goto done; /* full-box v0 flags0 */
        memcpy(t->audio_private, "fLaC", 4);
        if (io_read_data(info->handle, t->audio_private + 4, 38) == 38)
            t->audio_private_size = 42;
    }
done:
    /* A malformed or mismatched decoder box cannot be repaired by appending
     * another one: admission must not depend on which duplicate was valid. */
    if (!t->audio_private_size) info->parse_error = 1;
    (void)io_set_position64(info->handle, end);
}

static void read_mp4a_atom(mp4info_t* info, const uint64_t total_size) {
	int64_t dest_position;
	mp4info_track_t *track = info->tracks[info->total_tracks - 1];
	uint16_t sound_version;
	uint32_t sample_rate_fixed;
	int32_t i;

	/* ISO AudioSampleEntry fixed portion is 28 bytes after its atom header. */
	if (!atom_payload_end(info, total_size, &dest_position))
		return;
	if (total_size < 28) {
		io_set_position64(info->handle, dest_position);
		return;
	}

	for (i = 0; i < 6; ++i)
		io_read_8(info->handle);
	io_read_be16(info->handle); /* dataReferenceIndex */
	sound_version = io_read_be16(info->handle);
	io_read_be16(info->handle); /* revisionLevel */
	io_read_be32(info->handle); /* vendor */
	track->channels = io_read_be16(info->handle);
	track->samplebits = io_read_be16(info->handle);
	io_read_be16(info->handle); /* compressionId */
	io_read_be16(info->handle); /* packetSize */
	sample_rate_fixed = io_read_be32(info->handle);
	track->samplerate = sample_rate_fixed >> 16;

	/* QuickTime version-1 audio entries append four 32-bit packet fields. */
	if (sound_version == 1 && atom_has_bytes(info, dest_position, 16U)) {
		io_read_be32(info->handle);
		io_read_be32(info->handle);
		io_read_be32(info->handle);
		io_read_be32(info->handle);
	}

	/* Native .mp3/mp3 sample entries commonly have no esds child. */
	while (atom_has_bytes(info, dest_position, 8U)) {
		uint32_t atom_type = 0;
		uint32_t header_size = 0;
		uint64_t size = atom_read_header(info->handle, &atom_type, &header_size,
			(uint64_t)(dest_position - io_get_position64(info->handle)));
		uint64_t remaining = (uint64_t)(dest_position - io_get_position64(info->handle));

		if (size < header_size || size - header_size > remaining)
			break;
		if (atom_type == ATOM_TYPE('e','s','d','s'))
			read_esds_atom(info, size - header_size);
        else if (atom_type == ATOM_TYPE('d','O','p','s') ||
                 atom_type == ATOM_TYPE('d','f','L','a'))
            read_software_audio_config(info, atom_type, size - header_size);
		else
			parse_unused_atom(info, size - header_size);
	}

	io_set_position64(info->handle, dest_position);
}

static void read_avcC_atom(mp4info_t* info, const uint64_t total_size) {
    int64_t dest_position;
    mp4info_track_t *track = info->tracks[info->total_tracks - 1];
    uint8_t count;
    uint8_t value;
    uint32_t i;

    if (track->avc_sps || track->avc_pps) {
        /* One native AVC context owns one configuration. A repeated avcC
         * cannot replace live parser allocations or mix parameter sets. */
        info->parse_error = 1;
        return;
    }
    if (!atom_payload_end(info, total_size, &dest_position) ||
        !atom_has_bytes(info, dest_position, 7))
        return;
    if (io_read_8(info->handle) != 1)
        goto done;
    track->avc_profile = io_read_8(info->handle);
    (void)io_read_8(info->handle);
    (void)io_read_8(info->handle);
    value = io_read_8(info->handle);
    track->avc_nal_prefix_size = (value & 0x03U) + 1U;
    if (track->avc_nal_prefix_size == 3U)
        goto done;
    count = io_read_8(info->handle) & 0x1fU;
    if (count == 0)
        goto done;
    for (i = 0; i < count; ++i) {
        uint32_t size;
        if (!atom_has_bytes(info, dest_position, 2))
            goto done;
        size = io_read_be16(info->handle);
        if (size == 0 || size > MP4INFO_MAX_CODEC_CONFIG ||
            !atom_has_bytes(info, dest_position, size))
            goto done;
        if (i == 0) {
            track->avc_sps = atom_calloc_array(info, size, 1U, MP4INFO_MAX_CODEC_CONFIG);
            if (!track->avc_sps)
                goto done;
            if (io_read_data(info->handle, track->avc_sps, size) != size) {
                free(track->avc_sps); track->avc_sps = 0;
                goto done;
            }
            track->avc_sps_size = size;
        }
        else {
            parse_unused_atom(info, size);
        }
    }
    if (!atom_has_bytes(info, dest_position, 1))
        goto done;
    count = io_read_8(info->handle);
    if (count == 0)
        goto done;
    for (i = 0; i < count; ++i) {
        uint32_t size;
        if (!atom_has_bytes(info, dest_position, 2))
            goto done;
        size = io_read_be16(info->handle);
        if (size == 0 || size > MP4INFO_MAX_CODEC_CONFIG ||
            !atom_has_bytes(info, dest_position, size))
            goto done;
        if (i == 0) {
            track->avc_pps = atom_calloc_array(info, size, 1U, MP4INFO_MAX_CODEC_CONFIG);
            if (!track->avc_pps)
                goto done;
            if (io_read_data(info->handle, track->avc_pps, size) != size) {
                free(track->avc_pps); track->avc_pps = 0;
                goto done;
            }
            track->avc_pps_size = size;
        }
        else {
            parse_unused_atom(info, size);
        }
    }
done:
    (void)io_set_position64(info->handle, dest_position);
}

static void read_avc1_atom(mp4info_t* info, const uint64_t total_size) {
    int64_t dest_position;
    int32_t i;
    mp4info_track_t *track = info->tracks[info->total_tracks - 1];

    if (!atom_payload_end(info, total_size, &dest_position) ||
        !atom_has_bytes(info, dest_position, 78))
        return;
    for (i = 0; i < 6; ++i) (void)io_read_8(info->handle);
    (void)io_read_be16(info->handle);
    (void)io_read_be32(info->handle);
    (void)io_read_be32(info->handle);
    (void)io_read_be32(info->handle);
    (void)io_read_be32(info->handle);
    track->width = io_read_be16(info->handle);
    track->height = io_read_be16(info->handle);
    parse_unused_atom(info, 14 + 32 + 4);

    while (atom_has_bytes(info, dest_position, 8U)) {
        uint32_t atom_type = 0;
        uint32_t header_size = 0;
        uint64_t size = atom_read_header(info->handle, &atom_type, &header_size,
			(uint64_t)(dest_position - io_get_position64(info->handle)));
        uint64_t payload;
        if (size < header_size)
            break;
        payload = size - header_size;
        if (payload > (uint64_t)(dest_position - io_get_position64(info->handle)))
            break;
        if (atom_type == ATOM_TYPE('a','v','c','C'))
            read_avcC_atom(info, payload);
        else
            parse_unused_atom(info, payload);
    }
    (void)io_set_position64(info->handle, dest_position);
}

static void read_stsd_atom(mp4info_t* info, const uint64_t total_size) {
	int64_t dest_position;
	uint32_t stsd_entry_count;
	uint32_t i;
	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;
	mp4info_track_t *track = info->tracks[info->total_tracks - 1];

	if (!atom_payload_end(info, total_size, &dest_position) ||
	    !atom_has_bytes(info, dest_position, 8))
		return;

	io_read_8(info->handle);
	io_read_be24(info->handle);
	stsd_entry_count = io_read_be32(info->handle);
	if (stsd_entry_count > 256U)
		stsd_entry_count = 256U;

	for (i = 0; i < stsd_entry_count &&
	            atom_has_bytes(info, dest_position, 8U); i++) {
		size = atom_read_header(info->handle, &atom_type, &header_size,
			(uint64_t)(dest_position - io_get_position64(info->handle)));
		if (size < header_size ||
		    size - header_size > (uint64_t)(dest_position - io_get_position64(info->handle)))
			break;
		if (i == 0)
			track->sample_entry_type = atom_type;
		/* Playback has one codec configuration per track, not per chunk.
		 * Retain description one; stsc admission rejects switches to another. */
		if (i != 0) {
			parse_unused_atom(info, size - header_size);
		}
		else if (atom_type == ATOM_TYPE('m','p','4','a') ||
		    atom_type == ATOM_TYPE('.','m','p','3') ||
		    atom_type == ATOM_TYPE('m','p','3',' ') ||
            atom_type == ATOM_TYPE('f','L','a','C') ||
            atom_type == ATOM_TYPE('O','p','u','s')) {
			track->audio_type =
				(atom_type == ATOM_TYPE('.','m','p','3')) ?
                ATOM_TYPE('m','p','3',' ') : atom_type;
			read_mp4a_atom(info, size - header_size);
		}
		else if (atom_type == ATOM_TYPE('a','v','c','1') ||
		         atom_type == ATOM_TYPE('a','v','c','3')) {
			track->video_type = ATOM_TYPE('a','v','c','1');
			read_avc1_atom(info, size - header_size);
		}
		else {
			/* tx3g/text are retained as timed-text sample entries for QuickTime
			 * chapter extraction; their fixed sample-entry bodies are not needed. */
			parse_unused_atom(info, size - header_size);
		}
	}

	(void)io_set_position64(info->handle, dest_position);
}

static int atom_read_fullbox_count(mp4info_t *info, uint64_t total_size,
                                   int64_t *dest_position,
                                   uint8_t *version,
                                   uint32_t *count,
                                   uint32_t bytes_per_entry,
                                   uint32_t hard_limit)
{
    uint64_t required;
    if (!atom_payload_end(info, total_size, dest_position) ||
        !atom_has_bytes(info, *dest_position, 8))
        return 0;
    *version = io_read_8(info->handle);
    (void)io_read_be24(info->handle);
    *count = io_read_be32(info->handle);
    if (*count > hard_limit)
        return 0;
    required = (uint64_t)(*count) * bytes_per_entry;
    return required <= (uint64_t)(*dest_position - io_get_position64(info->handle));
}

static void read_stts_atom(mp4info_t* info, const uint64_t total_size) {
    int64_t dest_position = io_get_position64(info->handle);
    mp4info_track_t* track = info->tracks[info->total_tracks - 1];
    uint8_t version = 0;
    uint32_t count = 0;
    uint32_t i;

    if (!atom_read_fullbox_count(info, total_size, &dest_position, &version,
                                 &count, 8, MP4INFO_MAX_STTS_ENTRIES))
        goto done;
    (void)version;
    track->stts_sample_count = atom_calloc_array(info, count, sizeof(uint32_t),
                                                 MP4INFO_MAX_STTS_ENTRIES);
    track->stts_sample_duration = atom_calloc_array(info, count, sizeof(uint32_t),
                                                    MP4INFO_MAX_STTS_ENTRIES);
    if ((count != 0) && (!track->stts_sample_count || !track->stts_sample_duration))
        goto fail;
    for (i = 0; i < count; ++i) {
        track->stts_sample_count[i] = io_read_be32(info->handle);
        track->stts_sample_duration[i] = io_read_be32(info->handle);
        if (track->stts_sample_count[i] == 0)
            goto fail;
    }
    track->stts_entry_count = count;
    goto done;
fail:
    free(track->stts_sample_count); track->stts_sample_count = 0;
    free(track->stts_sample_duration); track->stts_sample_duration = 0;
    track->stts_entry_count = 0;
done:
    (void)io_set_position64(info->handle, dest_position);
}

static void read_ctts_atom(mp4info_t* info, const uint64_t total_size) {
    int64_t dest_position = io_get_position64(info->handle);
    mp4info_track_t* track = info->tracks[info->total_tracks - 1];
    uint8_t version = 0;
    uint32_t count = 0;
    uint32_t i;

    if (!atom_read_fullbox_count(info, total_size, &dest_position, &version,
                                 &count, 8, MP4INFO_MAX_STTS_ENTRIES) ||
        version > 1)
        goto done;
    track->ctts_sample_count = atom_calloc_array(info, count, sizeof(uint32_t),
                                                 MP4INFO_MAX_STTS_ENTRIES);
    track->ctts_sample_offset = atom_calloc_array(info, count, sizeof(int32_t),
                                                  MP4INFO_MAX_STTS_ENTRIES);
    if ((count != 0) && (!track->ctts_sample_count || !track->ctts_sample_offset))
        goto fail;
    for (i = 0; i < count; ++i) {
        uint32_t raw;
        track->ctts_sample_count[i] = io_read_be32(info->handle);
        raw = io_read_be32(info->handle);
        track->ctts_sample_offset[i] = version == 1 ? (int32_t)raw :
            (raw > 0x7fffffffU ? 0x7fffffff : (int32_t)raw);
        if (track->ctts_sample_count[i] == 0)
            goto fail;
    }
    track->ctts_version = version;
    track->ctts_entry_count = count;
    goto done;
fail:
    free(track->ctts_sample_count); track->ctts_sample_count = 0;
    free(track->ctts_sample_offset); track->ctts_sample_offset = 0;
    track->ctts_entry_count = 0;
done:
    (void)io_set_position64(info->handle, dest_position);
}

static void read_stss_atom(mp4info_t* info, const uint64_t total_size) {
    int64_t dest_position = io_get_position64(info->handle);
    mp4info_track_t* track = info->tracks[info->total_tracks - 1];
    uint8_t version = 0;
    uint32_t count = 0;
    uint32_t i;
    uint32_t previous = 0;

    if (!atom_read_fullbox_count(info, total_size, &dest_position, &version,
                                 &count, 4, MP4INFO_MAX_TABLE_ENTRIES) || version != 0)
        goto done;
    track->stss_sync_sample = atom_calloc_array(info, count, sizeof(uint32_t),
                                                MP4INFO_MAX_TABLE_ENTRIES);
    if (count != 0 && !track->stss_sync_sample)
        goto done;
    for (i = 0; i < count; ++i) {
        uint32_t sample = io_read_be32(info->handle);
        if (sample == 0 || (i != 0 && sample <= previous))
            goto fail;
        track->stss_sync_sample[i] = sample;
        previous = sample;
    }
    track->stss_entry_count = count;
    goto done;
fail:
    free(track->stss_sync_sample); track->stss_sync_sample = 0;
    track->stss_entry_count = 0;
done:
    (void)io_set_position64(info->handle, dest_position);
}

static void read_stsc_atom(mp4info_t* info, const uint64_t total_size) {
    int64_t dest_position = io_get_position64(info->handle);
    mp4info_track_t* track = info->tracks[info->total_tracks - 1];
    uint8_t version = 0;
    uint32_t count = 0;
    uint32_t i;
    uint32_t previous_chunk = 0;

    if (!atom_read_fullbox_count(info, total_size, &dest_position, &version,
                                 &count, 12, MP4INFO_MAX_TABLE_ENTRIES) ||
        version != 0 || count == 0 || count == 0xffffffffU)
        goto done;
    track->stsc_first_chunk = atom_calloc_array(info, count + 1U, sizeof(uint32_t),
                                                MP4INFO_MAX_TABLE_ENTRIES + 1U);
    track->stsc_samples_per_chunk = atom_calloc_array(info, count + 1U, sizeof(uint32_t),
                                                      MP4INFO_MAX_TABLE_ENTRIES + 1U);
    track->stsc_sample_desc_id = atom_calloc_array(info, count + 1U, sizeof(uint32_t),
                                                   MP4INFO_MAX_TABLE_ENTRIES + 1U);
    if (!track->stsc_first_chunk || !track->stsc_samples_per_chunk ||
        !track->stsc_sample_desc_id)
        goto fail;
    for (i = 0; i < count; ++i) {
        uint32_t first = io_read_be32(info->handle);
        uint32_t samples = io_read_be32(info->handle);
        uint32_t desc = io_read_be32(info->handle);
        if (first == 0 || (i == 0 && first != 1U) || samples == 0 || desc != 1U ||
            (i != 0 && first <= previous_chunk))
            goto fail;
        track->stsc_first_chunk[i] = first;
        track->stsc_samples_per_chunk[i] = samples;
        track->stsc_sample_desc_id[i] = desc;
        previous_chunk = first;
    }
    track->stsc_entry_count = count;
    goto done;
fail:
    free(track->stsc_first_chunk); track->stsc_first_chunk = 0;
    free(track->stsc_samples_per_chunk); track->stsc_samples_per_chunk = 0;
    free(track->stsc_sample_desc_id); track->stsc_sample_desc_id = 0;
    track->stsc_entry_count = 0;
done:
    (void)io_set_position64(info->handle, dest_position);
}

static void read_stsz_atom(mp4info_t* info, const uint64_t total_size) {
    int64_t dest_position;
    mp4info_track_t* track = info->tracks[info->total_tracks - 1];
    uint32_t i;
    uint32_t count;
    uint8_t version;

    if (!atom_payload_end(info, total_size, &dest_position) ||
        !atom_has_bytes(info, dest_position, 12))
        return;
    version = io_read_8(info->handle);
    (void)io_read_be24(info->handle);
    if (version != 0)
        goto done;
    track->stsz_sample_size = io_read_be32(info->handle);
    count = io_read_be32(info->handle);
    if (count > MP4INFO_MAX_TABLE_ENTRIES)
        goto done;
    if (track->stsz_sample_size == 0) {
        if ((uint64_t)count * 4U > (uint64_t)(dest_position - io_get_position64(info->handle)))
            goto done;
        track->stsz_sample_size_table = atom_calloc_array(info, count, sizeof(uint32_t),
                                                          MP4INFO_MAX_TABLE_ENTRIES);
        if (count != 0 && !track->stsz_sample_size_table)
            goto done;
        for (i = 0; i < count; ++i)
            track->stsz_sample_size_table[i] = io_read_be32(info->handle);
    }
    track->stsz_entry_count = count;
done:
    (void)io_set_position64(info->handle, dest_position);
}

static void read_chunk_offsets_atom(mp4info_t *info, const uint64_t total_size,
                                    int is_co64)
{
    int64_t dest_position = io_get_position64(info->handle);
    mp4info_track_t *track = info->tracks[info->total_tracks - 1];
    uint8_t version = 0;
    uint32_t count = 0;
    uint32_t i;
    uint32_t bytes = is_co64 ? 8U : 4U;

    if (!atom_read_fullbox_count(info, total_size, &dest_position, &version,
                                 &count, bytes, MP4INFO_MAX_TABLE_ENTRIES) || version != 0)
        goto done;
    track->stco_chunk_offset = atom_calloc_array(info, count, sizeof(uint64_t),
                                                 MP4INFO_MAX_TABLE_ENTRIES);
    if (count != 0 && !track->stco_chunk_offset)
        goto done;
    for (i = 0; i < count; ++i) {
        uint64_t offset = is_co64 ? io_read_be64(info->handle) : io_read_be32(info->handle);
        /* Both stco and co64 feed this wide representation. Native/VFS
         * positions remain signed-64; never narrow a 64-bit chunk address. */
        if (offset >= info->file_size)
            goto fail;
        track->stco_chunk_offset[i] = offset;
    }
    track->stco_entry_count = count;
    goto done;
fail:
    free(track->stco_chunk_offset); track->stco_chunk_offset = 0;
    track->stco_entry_count = 0;
done:
    (void)io_set_position64(info->handle, dest_position);
}

static void read_stco_atom(mp4info_t* info, const uint64_t total_size) {
    read_chunk_offsets_atom(info, total_size, 0);
}

static void read_co64_atom(mp4info_t* info, const uint64_t total_size) {
    read_chunk_offsets_atom(info, total_size, 1);
}

static void parse_stbl_atom(mp4info_t* info, const uint64_t total_size) {
	int64_t parent_start = io_get_position64(info->handle);
	uint64_t current_position = 0;
	mp4info_track_t *track = info->tracks[info->total_tracks - 1];

	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (!info->parse_error && total_size - current_position >= 8U) {
		int64_t child_start = io_get_position64(info->handle);
		uint32_t table_bit = 0;
		size = atom_read_header(info->handle, &atom_type, &header_size,
			total_size - current_position);
		if (size < header_size || size > total_size - current_position) {
			atom_finish_child(info->handle, child_start,
			                  total_size - current_position);
			break;
		}
		/* stco/co64 are alternative representations of the same table.
		 * Reject duplicates before allocating or replacing an owned pointer. */
		switch (atom_type) {
		case ATOM_TYPE('s','t','s','d'): table_bit = 1U << 0; break;
		case ATOM_TYPE('s','t','t','s'): table_bit = 1U << 1; break;
		case ATOM_TYPE('c','t','t','s'): table_bit = 1U << 2; break;
		case ATOM_TYPE('s','t','s','s'): table_bit = 1U << 3; break;
		case ATOM_TYPE('s','t','s','c'): table_bit = 1U << 4; break;
		case ATOM_TYPE('s','t','s','z'): table_bit = 1U << 5; break;
		case ATOM_TYPE('s','t','c','o'):
		case ATOM_TYPE('c','o','6','4'): table_bit = 1U << 6; break;
		default: break;
		}
		if (table_bit && (track->sample_tables_seen & table_bit)) {
			info->parse_error = 1;
			break;
		}
		track->sample_tables_seen |= table_bit;
		if (atom_type == ATOM_TYPE('s','t','s','d')) {
			read_stsd_atom(info, size - header_size);
		}
	 	else if (atom_type == ATOM_TYPE('s','t','t','s')) {
			read_stts_atom(info, size - header_size);
		}
	 	else if (atom_type == ATOM_TYPE('c','t','t','s')) {
			read_ctts_atom(info, size - header_size);
		}
	 	else if (atom_type == ATOM_TYPE('s','t','s','s')) {
			read_stss_atom(info, size - header_size);
		}
	 	else if (atom_type == ATOM_TYPE('s','t','s','c')) {
			read_stsc_atom(info, size - header_size);
		}
	 	else if (atom_type == ATOM_TYPE('s','t','s','z')) {
			read_stsz_atom(info, size - header_size);
		}
	 	else if (atom_type == ATOM_TYPE('s','t','c','o')) {
			read_stco_atom(info, size - header_size);
		}
		else if (atom_type == ATOM_TYPE('c','o','6','4')) {
			read_co64_atom(info, size - header_size);
		}
		else {
			parse_unused_atom(info, size - header_size);
		}
		atom_finish_child(info->handle, child_start, size);
		current_position += size;
	}
	atom_finish_child(info->handle, parent_start, total_size);
}

static void parse_minf_atom(mp4info_t* info, const uint64_t total_size) {
	int64_t parent_start = io_get_position64(info->handle);
	uint64_t current_position = 0;

	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (!info->parse_error && total_size - current_position >= 8U) {
		int64_t child_start = io_get_position64(info->handle);
		size = atom_read_header(info->handle, &atom_type, &header_size,
			total_size - current_position);
		if (size < header_size || size > total_size - current_position) {
			atom_finish_child(info->handle, child_start,
			                  total_size - current_position);
			break;
		}
		if (atom_type == ATOM_TYPE('v','m','h','d')) {
			info->tracks[info->total_tracks-1]->type = MP4_TRACK_VIDEO;
	 		parse_unused_atom(info, size - header_size);
		}
	 	else if (atom_type == ATOM_TYPE('s','m','h','d')) {
	 		info->tracks[info->total_tracks-1]->type = MP4_TRACK_AUDIO;
	 		parse_unused_atom(info, size - header_size);
		}
	 	else if (atom_type == ATOM_TYPE('s','t','b','l')) {
	 		parse_stbl_atom(info, size - header_size);
		}
		else {
			parse_unused_atom(info, size - header_size);
		}
		atom_finish_child(info->handle, child_start, size);
		current_position += size;
	}
	atom_finish_child(info->handle, parent_start, total_size);
}

static void read_hdlr_atom(mp4info_t* info, const uint64_t total_size) {
	int64_t dest_position;
	uint32_t sub_type;
	if (!atom_payload_end(info, total_size, &dest_position) ||
	    !atom_has_bytes(info, dest_position, 12))
		return;

	io_read_8(info->handle); //version
	io_read_be24(info->handle); //flags
	io_read_be32(info->handle); //Component Type;
	sub_type = io_read_be32(info->handle); //Component Subtype
	if ( sub_type == 0x7362746C || sub_type == 0x74657874 )
		info->tracks[info->total_tracks-1]->type = MP4_TRACK_SUBTITLE;
	io_set_position64(info->handle, dest_position);

}

static void read_mdhd_atom(mp4info_t* info, const uint64_t total_size) {
	int64_t dest_position;
	uint8_t version;
	mp4info_track_t *track = info->tracks[info->total_tracks - 1];

	if (!atom_payload_end(info, total_size, &dest_position) ||
	    !atom_has_bytes(info, dest_position, 4))
		return;
	version = io_read_8(info->handle);
	io_read_be24(info->handle);
	if (version == 1) {
		uint64_t duration;
		if (!atom_has_bytes(info, dest_position, 28))
			goto done;
		io_read_be64(info->handle);
		io_read_be64(info->handle);
		track->time_scale = io_read_be32(info->handle);
		duration = io_read_be64(info->handle);
		track->duration = duration > 0xffffffffULL ? 0xffffffffU : (uint32_t)duration;
	}
	else {
		if (!atom_has_bytes(info, dest_position, 16))
			goto done;
		io_read_be32(info->handle);
		io_read_be32(info->handle);
		track->time_scale = io_read_be32(info->handle);
		track->duration = io_read_be32(info->handle);
	}
done:
	(void)io_set_position64(info->handle, dest_position);
}

static void parse_mdia_atom(mp4info_t* info, const uint64_t total_size) {
	int64_t parent_start = io_get_position64(info->handle);
	uint64_t current_position = 0;

	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (!info->parse_error && total_size - current_position >= 8U) {
		int64_t child_start = io_get_position64(info->handle);
		size = atom_read_header(info->handle, &atom_type, &header_size,
			total_size - current_position);
		if (size < header_size || size > total_size - current_position) {
			atom_finish_child(info->handle, child_start,
			                  total_size - current_position);
			break;
		}
		if (atom_type == ATOM_TYPE('m','d','h','d')) {
			read_mdhd_atom(info, size - header_size);
		}
		else if (atom_type == ATOM_TYPE('m','i','n','f')) {
	 		parse_minf_atom(info, size - header_size);
		}
		else if (atom_type == ATOM_TYPE('h','d','l','r')) {
			read_hdlr_atom(info, size - header_size);
		}
		else {
			parse_unused_atom(info, size - header_size);
		}
		atom_finish_child(info->handle, child_start, size);
		current_position += size;
	}
	atom_finish_child(info->handle, parent_start, total_size);
}

static void read_tkhd_atom(mp4info_t* info, const uint64_t total_size) {
	int64_t dest_position;
	uint8_t version;
	mp4info_track_t *track = info->tracks[info->total_tracks - 1];

	if (!atom_payload_end(info, total_size, &dest_position) ||
	    !atom_has_bytes(info, dest_position, 4))
		return;
	version = io_read_8(info->handle);
	io_read_be24(info->handle);
	if (version == 1) {
		if (!atom_has_bytes(info, dest_position, 32))
			goto done;
		io_read_be64(info->handle);
		io_read_be64(info->handle);
		track->track_id = io_read_be32(info->handle);
		io_read_be32(info->handle);
		io_read_be64(info->handle);
	}
	else {
		if (!atom_has_bytes(info, dest_position, 20))
			goto done;
		io_read_be32(info->handle);
		io_read_be32(info->handle);
		track->track_id = io_read_be32(info->handle);
		io_read_be32(info->handle);
		io_read_be32(info->handle);
	}
	/* reserved[2], layer, alternate group, volume, reserved, matrix */
	if (!atom_has_bytes(info, dest_position, 52))
		goto done;
	parse_unused_atom(info, 52);
	if (atom_has_bytes(info, dest_position, 8)) {
		track->width = io_read_be32(info->handle) >> 16;
		track->height = io_read_be32(info->handle) >> 16;
	}
done:
	(void)io_set_position64(info->handle, dest_position);
}

static void parse_tref_atom(mp4info_t *info, const uint64_t total_size)
{
	int64_t end_position;
	mp4info_track_t *track = info->tracks[info->total_tracks - 1];
	if (!atom_payload_end(info, total_size, &end_position))
		return;
	while (atom_has_bytes(info, end_position, 8U)) {
		uint32_t type = 0;
		uint32_t header_size = 0;
		uint64_t size = atom_read_header(info->handle, &type, &header_size,
			(uint64_t)(end_position - io_get_position64(info->handle)));
		uint64_t payload;
		if (size < header_size)
			break;
		payload = size - header_size;
		if (payload > (uint64_t)(end_position - io_get_position64(info->handle)))
			break;
		if (type == ATOM_TYPE('c','h','a','p')) {
			while (payload >= 4 && track->chapter_ref_count < MP4_MAX_CHAPTER_REFS) {
				track->chapter_ref_ids[track->chapter_ref_count++] =
					io_read_be32(info->handle);
				payload -= 4;
			}
		}
		parse_unused_atom(info, payload);
	}
	(void)io_set_position64(info->handle, end_position);
}

static int add_track(mp4info_t* info) {
	mp4info_track_t* track;

	if (!info || info->total_tracks < 0 ||
	    info->total_tracks >= MP4_MAX_TRACKS)
		return 0;

	track = atom_calloc_array(info, 1U, sizeof(mp4info_track_t), 1U);
	if (!track)
		return 0;

	memset(track, 0, sizeof(mp4info_track_t));
	info->tracks[info->total_tracks++] = track;
	return 1;
}

/* Deliberately bounded edit support for new software audio codecs. Existing
 * AAC/video interpretation is unchanged. Repeats, gaps and rate edits reject
 * FLAC/Opus admission rather than inventing a presentation timeline. */
static void read_audio_edts(mp4info_t *info, uint64_t size)
{
    int64_t end;
    mp4info_track_t *t = info->tracks[info->total_tracks - 1];
    int seen = 0;
    if (!atom_payload_end(info, size, &end)) return;
    if (t->audio_edit_status) { t->audio_edit_status = -1; goto done; }
    t->audio_edit_status = -1;
    while (atom_has_bytes(info, end, 8)) {
        uint32_t type = 0, header = 0;
        uint64_t child = atom_read_header(info->handle, &type, &header,
                         (uint64_t)(end - io_get_position64(info->handle)));
        int64_t child_end;
        uint8_t version;
        uint64_t media_start, duration;
        if (child < header || child - header > (uint64_t)(end - io_get_position64(info->handle))) { t->audio_edit_status = -1; goto done; }
        child_end = io_get_position64(info->handle) + (int64_t)(child - header);
        if (type != ATOM_TYPE('e','l','s','t')) {
            (void)io_set_position64(info->handle, child_end); continue;
        }
        if (seen++ || child - header < 8) { t->audio_edit_status = -1; goto done; }
        version = io_read_8(info->handle);
        if (io_read_be24(info->handle) != 0 || io_read_be32(info->handle) != 1 ||
            version > 1 || !atom_has_bytes(info, child_end, version ? 20 : 12)) goto done;
        duration = version ? io_read_be64(info->handle) : io_read_be32(info->handle);
        media_start = version ? io_read_be64(info->handle) : io_read_be32(info->handle);
        if (media_start > (version ? INT64_MAX : INT32_MAX) ||
            io_read_be32(info->handle) != 0x00010000U || !duration) goto done;
        t->audio_edit_start = media_start; t->audio_edit_duration = duration;
        (void)io_set_position64(info->handle, child_end);
    }
    if (seen == 1 && io_get_position64(info->handle) == end) t->audio_edit_status = 1;
done:
    (void)io_set_position64(info->handle, end);
}

static void parse_trak_atom(mp4info_t* info, const uint64_t total_size) {
	int64_t parent_start = io_get_position64(info->handle);
	uint64_t current_position = 0;

	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (!info->parse_error && total_size - current_position >= 8U) {
		int64_t child_start = io_get_position64(info->handle);
		size = atom_read_header(info->handle, &atom_type, &header_size,
			total_size - current_position);
		if (size < header_size || size > total_size - current_position) {
			atom_finish_child(info->handle, child_start,
			                  total_size - current_position);
			break;
		}
		if (atom_type == ATOM_TYPE('t','k','h','d')) {
			read_tkhd_atom(info, size - header_size);
		}
		else if (atom_type == ATOM_TYPE('e','d','t','s')) {
            read_audio_edts(info, size - header_size);
        }
        else if (atom_type == ATOM_TYPE('t','r','e','f')) {
			parse_tref_atom(info, size - header_size);
		}
	 	else if (atom_type == ATOM_TYPE('m','d','i','a')) {
	 		parse_mdia_atom(info, size - header_size);
		}
		else {
			parse_unused_atom(info, size - header_size);
		}
		atom_finish_child(info->handle, child_start, size);
		current_position += size;
	}
	atom_finish_child(info->handle, parent_start, total_size);
}

static void read_mvhd_atom(mp4info_t* info, const uint64_t total_size) {
	int64_t dest_position;
	uint8_t version;
	if (!atom_payload_end(info, total_size, &dest_position) ||
	    !atom_has_bytes(info, dest_position, 4))
		return;
	version = io_read_8(info->handle);
	io_read_be24(info->handle);
	if (version == 1) {
		uint64_t duration;
		if (!atom_has_bytes(info, dest_position, 28))
			goto done;
		io_read_be64(info->handle);
		io_read_be64(info->handle);
		info->time_scale = (int32_t)io_read_be32(info->handle);
		duration = io_read_be64(info->handle);
		info->duration = duration > 0x7fffffffULL ? 0x7fffffff : (int32_t)duration;
	}
	else {
		if (!atom_has_bytes(info, dest_position, 16))
			goto done;
		io_read_be32(info->handle);
		io_read_be32(info->handle);
		info->time_scale = (int32_t)io_read_be32(info->handle);
		info->duration = (int32_t)io_read_be32(info->handle);
	}
done:
	(void)io_set_position64(info->handle, dest_position);
}

/*
 * Nero/ISO Base Media chapter list (normally moov/udta/chpl).
 *
 * The atom is a FullBox followed by an optional version-specific 32-bit
 * field, an 8-bit entry count, then entries containing a 64-bit timestamp in
 * 100 ns units and an 8-bit UTF-8 title length.  Keep parsing strictly bounded
 * by the atom payload so malformed optional metadata can never disturb the
 * movie/sample tables.
 */
static void parse_chpl_atom(mp4info_t *info, const uint64_t total_size)
{
	int64_t end_position;
	uint8_t version;
	uint8_t chapter_count;
	uint32_t i;
	uint32_t original_count;
	uint32_t parsed_count = 0;
	uint64_t previous_time_ms = 0;
	uint64_t movie_duration_ms = 0;
	int valid = 0;

	if (!atom_payload_end(info, total_size, &end_position))
		return;

	/* Prefer the first complete representation. A later duplicate chpl atom
	 * must not append a second copy or suppress a valid QuickTime chap track. */
	if (info->chapter_count != 0U)
		goto done;

	if (!atom_has_bytes(info, end_position, 5U))
		goto done;

	original_count = info->chapter_count;
	version = io_read_8(info->handle);
	(void)io_read_be24(info->handle); /* FullBox flags. */

	/* Nero chapter lists in the wild use FullBox version 0 or 1 only.
	 * Reject unknown layouts instead of interpreting arbitrary udta bytes as
	 * a chapter count. */
	if (version > 1U)
		goto rollback;

	if (version == 1U) {
		/* Version 1 carries one additional reserved/unknown word. */
		if (!atom_has_bytes(info, end_position, 4U))
			goto rollback;
		(void)io_read_be32(info->handle);
	}

	if (!atom_has_bytes(info, end_position, 1U))
		goto rollback;
	chapter_count = io_read_8(info->handle);

	if (chapter_count == 0U) {
		valid = 1;
		goto rollback;
	}

	/* Every entry requires at least an 8-byte timestamp and 1-byte title
	 * length. This catches the common false-positive layout immediately. */
	if (!atom_has_bytes(info, end_position, (uint32_t)chapter_count * 9U))
		goto rollback;

	if (info->time_scale > 0 && info->duration > 0)
		movie_duration_ms =
			(uint64_t)(uint32_t)info->duration * 1000ULL /
			(uint64_t)(uint32_t)info->time_scale;

	for (i = 0; i < (uint32_t)chapter_count; ++i) {
		uint64_t start_100ns;
		uint64_t time_ms;
		uint8_t title_size;
		uint32_t copy_size = 0;
		mp4info_chapter_t *chapter = 0;

		if (!atom_has_bytes(info, end_position, 9U))
			goto rollback;

		start_100ns = io_read_be64(info->handle);
		title_size = io_read_8(info->handle);
		if (!atom_has_bytes(info, end_position, title_size))
			goto rollback;

		time_ms = start_100ns / 10000ULL;

		/* chpl entries are timeline ordered. A backwards timestamp strongly
		 * indicates that this is not a valid chapter list. */
		if (parsed_count != 0U && time_ms < previous_time_ms)
			goto rollback;

		/* Permit one second of rounding slack, but reject metadata claiming a
		 * chapter far beyond the movie duration. */
		if (movie_duration_ms != 0U &&
		    time_ms > movie_duration_ms + 1000ULL)
			goto rollback;

		if (info->chapter_count < MP4_MAX_CHAPTERS) {
			chapter = &info->chapters[info->chapter_count];
			memset(chapter, 0, sizeof(*chapter));
			chapter->time_ms = time_ms;
			copy_size = title_size;
			if (copy_size >= sizeof(chapter->title))
				copy_size = sizeof(chapter->title) - 1U;
			if (copy_size != 0U &&
			    io_read_data(info->handle, (uint8_t *)chapter->title,
			                 copy_size) != copy_size)
				goto rollback;
			chapter->title[copy_size] = 0;
			if ((uint32_t)title_size > copy_size)
				parse_unused_atom(info, (uint32_t)title_size - copy_size);
			if (chapter->title[0] == 0)
				snprintf(chapter->title, sizeof(chapter->title),
				         "Chapter %lu",
				         (unsigned long)info->chapter_count + 1UL);
			info->chapter_count++;
		}
		else {
			/* Validate and consume excess spec-compliant entries even though the
			 * UI intentionally stores at most MP4_MAX_CHAPTERS. */
			parse_unused_atom(info, title_size);
		}

		previous_time_ms = time_ms;
		parsed_count++;
	}

	valid = parsed_count == (uint32_t)chapter_count;

rollback:
	if (!valid)
		info->chapter_count = original_count;

done:
	(void)io_set_position64(info->handle, end_position);
}

static void parse_udta_atom(mp4info_t *info, const uint64_t total_size)
{
	int64_t parent_start = io_get_position64(info->handle);
	uint64_t current_position = 0;
	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (!info->parse_error && total_size - current_position >= 8U) {
		int64_t child_start = io_get_position64(info->handle);
		size = atom_read_header(info->handle, &atom_type, &header_size,
			total_size - current_position);
		if (size < header_size || size > total_size - current_position) {
			atom_finish_child(info->handle, child_start,
			                  total_size - current_position);
			break;
		}
		if (atom_type == ATOM_TYPE('c','h','p','l'))
			parse_chpl_atom(info, size - header_size);
		else
			parse_unused_atom(info, size - header_size);
		atom_finish_child(info->handle, child_start, size);
		current_position += size;
	}
	atom_finish_child(info->handle, parent_start, total_size);
}

static void parse_moov_atom(mp4info_t* info, const uint64_t total_size) {
	int64_t parent_start = io_get_position64(info->handle);
	uint64_t current_position = 0;

	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (!info->parse_error && total_size - current_position >= 8U) {
		int64_t child_start = io_get_position64(info->handle);
		size = atom_read_header(info->handle, &atom_type, &header_size,
			total_size - current_position);
		if (size < header_size || size > total_size - current_position) {
			atom_finish_child(info->handle, child_start,
			                  total_size - current_position);
			break;
		}
		if (atom_type == ATOM_TYPE('m','v','h','d')) {
			read_mvhd_atom(info, size - header_size);
		}
		else if (atom_type == ATOM_TYPE('t','r','a','k')) {
			if (add_track(info))
				parse_trak_atom(info, size - header_size);
			else
				parse_unused_atom(info, size - header_size);
		}
		else if (atom_type == ATOM_TYPE('u','d','t','a')) {
			parse_udta_atom(info, size - header_size);
		}
		else if (atom_type == ATOM_TYPE('c','h','p','l')) {
			/* Some muxers place chpl directly under moov. */
			parse_chpl_atom(info, size - header_size);
		}
		else {
			parse_unused_atom(info, size - header_size);
		}
		atom_finish_child(info->handle, child_start, size);
		current_position += size;
	}
	atom_finish_child(info->handle, parent_start, total_size);
}

void parse_atoms(mp4info_t* info) {
	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (!info->parse_error && io_get_position64(info->handle) >= 0 &&
	       io_get_length64(info->handle) - io_get_position64(info->handle) >= 8) {
		int64_t atom_start = io_get_position64(info->handle);
		size = atom_read_header(info->handle, &atom_type, &header_size,
		                        (uint64_t)(io_get_length64(info->handle) -
		                                   atom_start));
		if (size == 0)
			break;
		if (atom_type == ATOM_TYPE('m','o','o','v') && size > header_size) {
			parse_moov_atom(info, size - header_size);
		}
		else {
			parse_unused_atom(info, size - header_size);
		}
		atom_finish_child(info->handle, atom_start, size);
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////
// META parse
/////////////////////////////////////////////////////////////////////////////////////////////////////////

static void parse_meta_unused_atom(mp4meta_t* meta, const uint64_t total_size) {
	atom_finish_child(meta->handle, io_get_position64(meta->handle), total_size);
}

#define MP4_META_TEXT_MAX 4096U

static void read_meta_text_atom(mp4meta_t *meta, char **destination,
                                uint64_t total_size)
{
    int64_t start;
    int64_t end;
    uint32_t text_size;
    char *text = 0;
    if (meta == 0 || meta->handle == 0 || destination == 0)
        return;
    start = io_get_position64(meta->handle);
    if (!atom_range_end(meta->handle, start, total_size, &end)) return;
    if (total_size < 8U) {
        (void)io_set_position64(meta->handle, end);
        return;
    }
    (void)io_read_be32(meta->handle); /* data type/flags */
    (void)io_read_be32(meta->handle); /* locale */
    text_size = total_size - 8U >= MP4_META_TEXT_MAX ?
                MP4_META_TEXT_MAX - 1U : (uint32_t)(total_size - 8U);
    text = (char *)malloc((size_t)text_size + 1U);
    if (text != 0) {
        uint32_t got = io_read_data(meta->handle, (uint8_t *)text, text_size);
        text[got < text_size ? got : text_size] = 0;
        free(*destination);
        *destination = text;
    }
    (void)io_set_position64(meta->handle, end);
}

static void read_meta_nam_atom(mp4meta_t* meta, const uint64_t total_size) {
    read_meta_text_atom(meta, &meta->title, total_size);
}

static void read_meta_art_atom(mp4meta_t* meta, const uint64_t total_size) {
    read_meta_text_atom(meta, &meta->artist, total_size);
}

static void read_meta_alb_atom(mp4meta_t* meta, const uint64_t total_size) {
    read_meta_text_atom(meta, &meta->album, total_size);
}

static void parse_meta_nam_atom(mp4meta_t* meta, const uint64_t total_size) {
	int64_t parent_start = io_get_position64(meta->handle);
	uint64_t current_position = 0;

	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (total_size - current_position >= 8U) {
		int64_t child_start = io_get_position64(meta->handle);
		size = atom_read_header(meta->handle, &atom_type, &header_size,
			total_size - current_position);
		if (size < header_size || size > total_size - current_position) {
			atom_finish_child(meta->handle, child_start,
			                  total_size - current_position);
			break;
		}
		if (atom_type == ATOM_TYPE('d','a','t','a')) {
			read_meta_nam_atom(meta, size - header_size);
		}
		else {
			parse_meta_unused_atom(meta, size - header_size);
		}
		atom_finish_child(meta->handle, child_start, size);
		current_position += size;
	}
	atom_finish_child(meta->handle, parent_start, total_size);
}

static void parse_meta_art_atom(mp4meta_t* meta, const uint64_t total_size) {
	int64_t parent_start = io_get_position64(meta->handle);
	uint64_t current_position = 0;

	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (total_size - current_position >= 8U) {
		int64_t child_start = io_get_position64(meta->handle);
		size = atom_read_header(meta->handle, &atom_type, &header_size,
			total_size - current_position);
		if (size < header_size || size > total_size - current_position) {
			atom_finish_child(meta->handle, child_start,
			                  total_size - current_position);
			break;
		}
		if (atom_type == ATOM_TYPE('d','a','t','a')) {
			read_meta_art_atom(meta, size - header_size);
		}
		else {
			parse_meta_unused_atom(meta, size - header_size);
		}
		atom_finish_child(meta->handle, child_start, size);
		current_position += size;
	}
	atom_finish_child(meta->handle, parent_start, total_size);
}

static void parse_meta_alb_atom(mp4meta_t* meta, const uint64_t total_size) {
	int64_t parent_start = io_get_position64(meta->handle);
	uint64_t current_position = 0;

	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (total_size - current_position >= 8U) {
		int64_t child_start = io_get_position64(meta->handle);
		size = atom_read_header(meta->handle, &atom_type, &header_size,
			total_size - current_position);
		if (size < header_size || size > total_size - current_position) {
			atom_finish_child(meta->handle, child_start,
			                  total_size - current_position);
			break;
		}
		if (atom_type == ATOM_TYPE('d','a','t','a')) {
			read_meta_alb_atom(meta, size - header_size);
		}
		else {
			parse_meta_unused_atom(meta, size - header_size);
		}
		atom_finish_child(meta->handle, child_start, size);
		current_position += size;
	}
	atom_finish_child(meta->handle, parent_start, total_size);
}

static void parse_meta_ilst_atom(mp4meta_t* meta, const uint64_t total_size) {
	int64_t parent_start = io_get_position64(meta->handle);
	uint64_t current_position = 0;

	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (total_size - current_position >= 8U) {
		int64_t child_start = io_get_position64(meta->handle);
		size = atom_read_header(meta->handle, &atom_type, &header_size,
			total_size - current_position);
		if (size < header_size || size > total_size - current_position) {
			atom_finish_child(meta->handle, child_start,
			                  total_size - current_position);
			break;
		}
		if (atom_type == ATOM_TYPE(0xA9, 'n', 'a', 'm')) {
			parse_meta_nam_atom(meta, size - header_size);
		}
		else if (atom_type == ATOM_TYPE(0xA9, 'A', 'R', 'T')) {
			parse_meta_art_atom(meta, size - header_size);
		}
		else if (atom_type == ATOM_TYPE(0xA9, 'a', 'l', 'b')) {
			parse_meta_alb_atom(meta, size - header_size);
		}
		else {
			parse_meta_unused_atom(meta, size - header_size);
		}
		atom_finish_child(meta->handle, child_start, size);
		current_position += size;
	}
	atom_finish_child(meta->handle, parent_start, total_size);
}

static void parse_meta_meta_atom(mp4meta_t* meta, const uint64_t total_size) {
	int64_t parent_start = io_get_position64(meta->handle);
	if (total_size < 4U)
		return;
	io_read_be32(meta->handle);

	uint64_t current_position = 4;

	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (total_size - current_position >= 8U) {
		int64_t child_start = io_get_position64(meta->handle);
		size = atom_read_header(meta->handle, &atom_type, &header_size,
			total_size - current_position);
		if (size < header_size || size > total_size - current_position) {
			atom_finish_child(meta->handle, child_start,
			                  total_size - current_position);
			break;
		}
		if (atom_type == ATOM_TYPE('i','l','s','t')) {
			parse_meta_ilst_atom(meta, size - header_size);
		}
		else {
			parse_meta_unused_atom(meta, size - header_size);
		}
		atom_finish_child(meta->handle, child_start, size);
		current_position += size;
	}
	atom_finish_child(meta->handle, parent_start, total_size);
}

static void parse_meta_udta_atom(mp4meta_t* meta, const uint64_t total_size) {
	int64_t parent_start = io_get_position64(meta->handle);
	uint64_t current_position = 0;

	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (total_size - current_position >= 8U) {
		int64_t child_start = io_get_position64(meta->handle);
		size = atom_read_header(meta->handle, &atom_type, &header_size,
			total_size - current_position);
		if (size < header_size || size > total_size - current_position) {
			atom_finish_child(meta->handle, child_start,
			                  total_size - current_position);
			break;
		}
		if (atom_type == ATOM_TYPE('m','e','t','a')) {
			parse_meta_meta_atom(meta, size - header_size);
		}
		else {
			parse_meta_unused_atom(meta, size - header_size);
		}
		atom_finish_child(meta->handle, child_start, size);
		current_position += size;
	}
	atom_finish_child(meta->handle, parent_start, total_size);
}

static void parse_meta_moov_atom(mp4meta_t* meta, const uint64_t total_size) {
	int64_t parent_start = io_get_position64(meta->handle);
	uint64_t current_position = 0;

	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (total_size - current_position >= 8U) {
		int64_t child_start = io_get_position64(meta->handle);
		size = atom_read_header(meta->handle, &atom_type, &header_size,
			total_size - current_position);
		if (size < header_size || size > total_size - current_position) {
			atom_finish_child(meta->handle, child_start,
			                  total_size - current_position);
			break;
		}
		if (atom_type == ATOM_TYPE('u','d','t','a')) {
			parse_meta_udta_atom(meta, size - header_size);
		}
		else {
			parse_meta_unused_atom(meta, size - header_size);
		}
		atom_finish_child(meta->handle, child_start, size);
		current_position += size;
	}
	atom_finish_child(meta->handle, parent_start, total_size);
}

void parse_metas(mp4meta_t* meta) {
	uint32_t atom_type = 0;
	uint32_t header_size = 0;
	uint64_t size = 0;

	while (io_get_position64(meta->handle) >= 0 &&
	       io_get_length64(meta->handle) - io_get_position64(meta->handle) >= 8) {
		int64_t atom_start = io_get_position64(meta->handle);
		size = atom_read_header(meta->handle, &atom_type, &header_size,
		                        (uint64_t)(io_get_length64(meta->handle) -
		                                   atom_start));
		if (size == 0)
			break;
		if (atom_type == ATOM_TYPE('m','o','o','v') && size > header_size) {
			parse_meta_moov_atom(meta, size - header_size);
		}
		else {
			parse_meta_unused_atom(meta, size - header_size);
		}
		atom_finish_child(meta->handle, atom_start, size);
	}
}
