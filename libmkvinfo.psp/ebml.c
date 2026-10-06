/* ebml.c
 * Copyright (C) 2009 cooleyes
 *
 * PSP-oriented EBML/Matroska metadata parser hardening.
 *
 * This parser intentionally supports a bounded PSP-useful Matroska subset.
 * It recognizes CRC-32/Void structurally and skips them without calculating
 * CRC checksums. It rejects or ignores features that are not safe/useful for
 * PSP playback, especially encryption and non-header-stripping compression.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

#include "bufferedio.h"
#include "mkvinfo_type.h"
#include "ebml_id.h"

#define FOURCC(a,b,c,d) \
	(((uint32_t)(d)) | (((uint32_t)(c)) << 8) | (((uint32_t)(b)) << 16) | (((uint32_t)(a)) << 24))

#define MKVINFO_MAX_CODEC_PRIVATE          (16U * 1024U)
#define MKVINFO_MAX_COMPRESSION_SETTINGS   (16U * 1024U)
#define MKVINFO_MAX_STRING_SIZE            (4U * 1024U)
#define MKVINFO_MAX_CUES                   20000
#define MKVINFO_MAX_SEEKHEAD_VISITS        32
#define MKVINFO_MAX_CUE_TRACK_POSITIONS    8

static double int2double(int64_t v)
{
	double out;
	uint64_t bits = (uint64_t)v;
	memcpy(&out, &bits, sizeof(out));
	return out;
}

static float int2float(int32_t v)
{
	float out;
	uint32_t bits = (uint32_t)v;
	memcpy(&out, &bits, sizeof(out));
	return out;
}

static int ebml_uint64_to_int32(uint64_t v, int32_t *out)
{
	if (out == 0)
		return 0;

	if (v > 0x7fffffffULL)
		return 0;

	*out = (int32_t)v;
	return 1;
}

static int ebml_uint64_to_uint32(uint64_t v, uint32_t *out)
{
	if (out == 0)
		return 0;

	if (v > 0xffffffffULL)
		return 0;

	*out = (uint32_t)v;
	return 1;
}

static int ebml_calc_end64(void *handle, uint64_t payload_size, int64_t *end_out)
{
	int64_t pos;
	int64_t file_len;

	if (handle == 0 || end_out == 0)
		return 0;

	if (payload_size > (uint64_t)INT64_MAX)
		return 0;

	pos = io_get_position64(handle);
	if (pos < 0)
		return 0;

	if (pos > INT64_MAX - (int64_t)payload_size)
		return 0;

	*end_out = pos + (int64_t)payload_size;

	file_len = io_get_length64(handle);
	if (file_len > 0 && *end_out > file_len)
		return 0;

	return 1;
}

static int ebml_skip_payload(void *handle, uint64_t payload_size)
{
	int64_t target;

	if (!ebml_calc_end64(handle, payload_size, &target))
		return 1;

	if (io_set_position64(handle, target) != target)
		return 1;

	return 0;
}

static int ebml_consume_child(uint64_t *parent_remaining,
                              int32_t id_len,
                              uint64_t child_len_with_size_field)
{
	uint64_t used;

	if (parent_remaining == 0)
		return 0;

	if (id_len <= 0)
		return 0;

	if (child_len_with_size_field > UINT64_MAX - (uint64_t)id_len)
		return 0;

	used = (uint64_t)id_len + child_len_with_size_field;

	if (*parent_remaining < used)
		return 0;

	*parent_remaining -= used;
	return 1;
}

static int ebml_ascii_tolower(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c + ('a' - 'A');

	return c;
}

static int ebml_ascii_ieq(const char *a, const char *b)
{
	if (a == 0 || b == 0)
		return 0;

	while (*a && *b) {
		if (ebml_ascii_tolower((unsigned char)*a) !=
		    ebml_ascii_tolower((unsigned char)*b))
			return 0;

		a++;
		b++;
	}

	return *a == 0 && *b == 0;
}

static int ebml_ascii_iends_with(const char *s, const char *suffix)
{
	size_t slen;
	size_t tlen;

	if (s == 0 || suffix == 0)
		return 0;

	slen = strlen(s);
	tlen = strlen(suffix);

	if (tlen > slen)
		return 0;

	return ebml_ascii_ieq(s + slen - tlen, suffix);
}

static int mkvinfo_attachment_is_font(const char *name, const char *mime)
{
	if (mime != 0 && mime[0] != '\0') {
		if (ebml_ascii_ieq(mime, "application/x-truetype-font") ||
		    ebml_ascii_ieq(mime, "application/x-font-ttf") ||
		    ebml_ascii_ieq(mime, "font/ttf") ||
		    ebml_ascii_ieq(mime, "application/vnd.ms-opentype") ||
		    ebml_ascii_ieq(mime, "application/x-font-otf") ||
		    ebml_ascii_ieq(mime, "font/otf") ||
		    ebml_ascii_ieq(mime, "application/font-sfnt"))
			return 1;
	}

	if (name != 0 && name[0] != '\0') {
		if (ebml_ascii_iends_with(name, ".ttf") ||
		    ebml_ascii_iends_with(name, ".ttc") ||
		    ebml_ascii_iends_with(name, ".otf") ||
		    ebml_ascii_iends_with(name, ".otc"))
			return 1;
	}

	return 0;
}

static uint32_t ebml_read_id(void *handle, int32_t *length)
{
	int32_t i;
	int32_t len_mask = 0x80;
	uint32_t id;
	uint8_t byte;

	if (length) *length = 0;
	if (io_read_data(handle, &byte, 1) != 1)
		return EBML_ID_INVALID;
	id = byte;

	for (i = 0; i < 4 && !(id & len_mask); i++)
		len_mask >>= 1;

	if (i >= 4)
		return EBML_ID_INVALID;

	if (length)
		*length = i + 1;

	while (i--) {
		if (io_read_data(handle, &byte, 1) != 1)
			return EBML_ID_INVALID;
		id = (id << 8) | byte;
	}

	return id;
}

uint64_t ebml_read_length(void *handle, int32_t *length)
{
	int32_t i;
	int32_t j;
	int32_t num_ffs = 0;
	int32_t len_mask = 0x80;
	uint64_t len;
	uint8_t byte;

	if (length) *length = 0;
	if (io_read_data(handle, &byte, 1) != 1)
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

	if ((int32_t)len == len_mask - 1)
		num_ffs++;

	while (i--) {
		if (io_read_data(handle, &byte, 1) != 1)
			return EBML_UINT_INVALID;
		len = (len << 8) | byte;

		if ((len & 0xFF) == 0xFF)
			num_ffs++;
	}

	if (j == num_ffs)
		return EBML_UINT_INVALID;

	return len;
}

uint64_t ebml_read_uint(void *handle, uint64_t *length)
{
	uint64_t len;
	uint64_t value = 0;
	int32_t l;
	uint8_t byte;

	len = ebml_read_length(handle, &l);
	if (len == EBML_UINT_INVALID || len < 1 || len > 8)
		return EBML_UINT_INVALID;

	if (length)
		*length = len + l;

	while (len--) {
		if (io_read_data(handle, &byte, 1) != 1)
			return EBML_UINT_INVALID;
		value = (value << 8) | byte;
	}

	return value;
}

int64_t ebml_read_int(void *handle, uint64_t *length)
{
	uint64_t bits = 0;
	int64_t value;
	uint64_t len;
	uint64_t bytes;
	int32_t l;
	uint8_t byte;

	len = ebml_read_length(handle, &l);
	if (len == EBML_UINT_INVALID || len < 1 || len > 8)
		return EBML_INT_INVALID;

	if (length)
		*length = len + l;

	bytes = len;
	while (len--) {
		if (io_read_data(handle, &byte, 1) != 1)
			return EBML_INT_INVALID;
		bits = (bits << 8) | byte;
	}
	/* Sign-extend in unsigned arithmetic; shifting a negative signed value
	 * is undefined on the Allegrex toolchain just as it is in ISO C. */
	if (bytes < 8 && (bits & (1ULL << (bytes * 8U - 1U))))
		bits |= UINT64_MAX << (bytes * 8U);
	memcpy(&value, &bits, sizeof(value));

	return value;
}

long double ebml_read_float(void *handle, uint64_t *length)
{
	long double value;
	uint64_t len;
	int32_t l;
	uint8_t bytes[8];
	uint64_t bits = 0;
	unsigned int i;

	len = ebml_read_length(handle, &l);
	if ((len != 4 && len != 8) ||
	    io_read_data(handle, bytes, (uint32_t)len) != len)
		return EBML_FLOAT_INVALID;
	for (i = 0; i < (unsigned int)len; ++i)
		bits = (bits << 8) | bytes[i];

	switch (len) {
		case 4:
			value = int2float((int32_t)bits);
			break;

		case 8:
			value = int2double((int64_t)bits);
			break;

		default:
			return EBML_FLOAT_INVALID;
	}

	if (length)
		*length = len + l;

	return value;
}

char *ebml_read_ascii(void *handle, uint64_t *length)
{
	uint64_t len;
	char *str = NULL;
	int32_t l;

	len = ebml_read_length(handle, &l);
	if (len == EBML_UINT_INVALID)
		return NULL;

	if (len > MKVINFO_MAX_STRING_SIZE)
		return NULL;

	if (len > SIZE_MAX - 1)
		return NULL;

	if (length)
		*length = len + l;

	str = (char *)malloc((size_t)len + 1);
	if (!str)
		return NULL;

	if (io_read_data(handle, (uint8_t *)str, len) != len) {
		free(str);
		return NULL;
	}

	str[len] = '\0';
	return str;
}

char *ebml_read_utf8(void *handle, uint64_t *length)
{
	return ebml_read_ascii(handle, length);
}

int32_t ebml_read_skip(void *handle, uint64_t *length)
{
	uint64_t len;
	int32_t l;

	len = ebml_read_length(handle, &l);
	if (len == EBML_UINT_INVALID)
		return 1;

	if (length)
		*length = len + l;

	return ebml_skip_payload(handle, len);
}

uint32_t ebml_read_master(void *handle, uint64_t *length)
{
	uint64_t len;
	uint32_t id;

	id = ebml_read_id(handle, NULL);
	if (id == EBML_ID_INVALID)
		return id;

	len = ebml_read_length(handle, NULL);
	if (len == EBML_UINT_INVALID)
		return EBML_ID_INVALID;

	if (length)
		*length = len;

	return id;
}

char *ebml_read_header(void *handle, int32_t *version)
{
	uint64_t length;
	uint64_t l;
	uint64_t num;
	uint32_t id;
	char *str = NULL;
	int32_t il;

	if (ebml_read_master(handle, &length) != EBML_ID_HEADER)
		return NULL;

	if (version)
		*version = 1;

	while (length > 0) {
		id = ebml_read_id(handle, &il);
		if (id == EBML_ID_INVALID)
			goto err_out;

		if ((uint64_t)il > length)
			goto err_out;

		switch (id) {
			case EBML_ID_EBMLREADVERSION:
				num = ebml_read_uint(handle, &l);
				if (num != EBML_VERSION)
					goto err_out;
				break;

			case EBML_ID_EBMLMAXSIZELENGTH:
				num = ebml_read_uint(handle, &l);
				if (num == EBML_UINT_INVALID || num < 1 || num > 8)
					goto err_out;
				break;

			case EBML_ID_EBMLMAXIDLENGTH:
				num = ebml_read_uint(handle, &l);
				if (num == EBML_UINT_INVALID || num < 1 || num > 4)
					goto err_out;
				break;

			case EBML_ID_DOCTYPE:
				if (str != NULL) {
					free(str);
					str = NULL;
				}

				str = ebml_read_ascii(handle, &l);
				if (str == NULL)
					goto err_out;
				break;

			case EBML_ID_DOCTYPEREADVERSION:
				num = ebml_read_uint(handle, &l);
				if (num == EBML_UINT_INVALID)
					goto err_out;

				if (version)
					*version = (int32_t)num;
				break;

			case EBML_ID_CRC32:
			case EBML_ID_VOID:
			case EBML_ID_EBMLVERSION:
			case EBML_ID_DOCTYPEVERSION:
			default:
				if (ebml_read_skip(handle, &l))
					goto err_out;
				break;
		}

		if (!ebml_consume_child(&length, il, l))
			goto err_out;
	}

	return str;

err_out:
	if (str != NULL)
		free(str);

	return NULL;
}

static int32_t parse_ebml_info(mkvinfo_t *info)
{
	uint64_t length;
	uint64_t l;
	int32_t il;
	uint64_t tc_scale = 1000000;
	long double duration = 0.0;

	length = ebml_read_length(info->handle, NULL);
	if (length == EBML_UINT_INVALID)
		return 1;

	while (length > 0) {
		uint32_t id = ebml_read_id(info->handle, &il);
		if (id == EBML_ID_INVALID)
			return 1;

		switch (id) {
			case MATROSKA_ID_TIMECODESCALE: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID || num == 0)
					return 1;

				tc_scale = num;
				break;
			}

			case MATROSKA_ID_DURATION: {
				long double num = ebml_read_float(info->handle, &l);
				if (num == EBML_FLOAT_INVALID || num != num || num < 0.0)
					return 1;

				duration = num;
				break;
			}

			case EBML_ID_CRC32:
			case EBML_ID_VOID:
			default:
				if (ebml_read_skip(info->handle, &l))
					return 1;
				break;
		}

		if (!ebml_consume_child(&length, il, l))
			return 1;
	}

	info->timecode_scale = tc_scale;
	duration = duration * tc_scale / 1000000.0;
	/* Avoid undefined floating-to-integer conversion for NaN/infinity and
	 * reject ranges the signed-millisecond playback API cannot express. */
	if (duration != duration || duration < 0.0 || duration > INT_MAX)
		return 1;
	info->duration = (uint64_t)duration;

	return 0;
}

static void mkv_free_trackentry(mkvinfo_track_t *track)
{
	if (track) {
		if (track->private_data)
			free(track->private_data);

		if (track->compress_setting)
			free(track->compress_setting);

		free(track);
	}
}

static int32_t parse_ebml_trackaudio(mkvinfo_t *info,
                                     mkvinfo_track_t *track)
{
	uint64_t len;
	uint64_t length;
	uint64_t l;
	int32_t il;

	len = length = ebml_read_length(info->handle, &il);
	if (len == EBML_UINT_INVALID)
		return 0;

	len += il;

	while (length > 0) {
		uint32_t id = ebml_read_id(info->handle, &il);
		if (id == EBML_ID_INVALID)
			return 0;

		switch (id) {
			case MATROSKA_ID_AUDIOSAMPLINGFREQ: {
				long double num = ebml_read_float(info->handle, &l);
				if (num == EBML_FLOAT_INVALID || num != num ||
				    num <= 0.0 || num > UINT32_MAX)
					return 0;

				track->samplerate = (uint32_t)num;
				break;
			}

			case MATROSKA_ID_AUDIOBITDEPTH: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					return 0;

				if (!ebml_uint64_to_uint32(num, &track->samplebits))
					return 0;

				break;
			}

			case MATROSKA_ID_AUDIOCHANNELS: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					return 0;

				if (!ebml_uint64_to_uint32(num, &track->channels))
					return 0;

				break;
			}

			case EBML_ID_CRC32:
			case EBML_ID_VOID:
			default:
				if (ebml_read_skip(info->handle, &l))
					return 0;
				break;
		}

		if (!ebml_consume_child(&length, il, l))
			return 0;
	}

	return len;
}

static int32_t parse_ebml_trackvideo(mkvinfo_t *info,
                                     mkvinfo_track_t *track)
{
	uint64_t len;
	uint64_t length;
	uint64_t l;
	int32_t il;

	len = length = ebml_read_length(info->handle, &il);
	if (len == EBML_UINT_INVALID)
		return 0;

	len += il;

	while (length > 0) {
		uint32_t id = ebml_read_id(info->handle, &il);
		if (id == EBML_ID_INVALID)
			return 0;

		switch (id) {
			case MATROSKA_ID_VIDEODISPLAYWIDTH: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					return 0;

				if (!ebml_uint64_to_uint32(num, &track->display_width))
					return 0;

				break;
			}

			case MATROSKA_ID_VIDEODISPLAYHEIGHT: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					return 0;

				if (!ebml_uint64_to_uint32(num, &track->display_height))
					return 0;

				break;
			}

			case MATROSKA_ID_VIDEOPIXELWIDTH: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					return 0;

				if (!ebml_uint64_to_uint32(num, &track->width))
					return 0;

				break;
			}

			case MATROSKA_ID_VIDEOPIXELHEIGHT: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					return 0;

				if (!ebml_uint64_to_uint32(num, &track->height))
					return 0;

				break;
			}

			case EBML_ID_CRC32:
			case EBML_ID_VOID:
			default:
				if (ebml_read_skip(info->handle, &l))
					return 0;
				break;
		}

		if (!ebml_consume_child(&length, il, l))
			return 0;
	}

	return len;
}

static int32_t parse_ebml_contentcompression(mkvinfo_t *info,
                                             mkvinfo_track_t *track)
{
	uint64_t len;
	uint64_t length;
	uint64_t l;
	int32_t il;
	int have_algo = 0;
	uint64_t algo = 0;

	len = length = ebml_read_length(info->handle, &il);
	if (len == EBML_UINT_INVALID)
		return 0;

	len += il;

	while (length > 0) {
		uint32_t id = ebml_read_id(info->handle, &il);
		if (id == EBML_ID_INVALID)
			return 0;

		switch (id) {
			case MATROSKA_ID_CONTENTCOMPALGO: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					return 0;

				algo = num;
				have_algo = 1;
				break;
			}

			case MATROSKA_ID_CONTENTCOMPSETTINGS: {
				int32_t size_len;
				uint64_t payload_size;

				payload_size = ebml_read_length(info->handle, &size_len);
				if (payload_size == EBML_UINT_INVALID)
					return 0;

				if (payload_size > MKVINFO_MAX_COMPRESSION_SETTINGS)
					return 0;

				if (payload_size > SIZE_MAX - MKV_INPUT_PADDING)
					return 0;

				l = payload_size + size_len;

				if (track->compress_setting) {
					free(track->compress_setting);
					track->compress_setting = 0;
					track->compress_setting_size = 0;
				}

				track->compress_setting =
					(uint8_t *)malloc((size_t)payload_size + MKV_INPUT_PADDING);
				if (!track->compress_setting)
					return 0;

				if (io_read_data(info->handle,
				                 track->compress_setting,
				                 payload_size) != payload_size)
					return 0;

				memset(track->compress_setting + payload_size,
				       0,
				       MKV_INPUT_PADDING);

				track->compress_setting_size = (uint32_t)payload_size;
				break;
			}

			case EBML_ID_CRC32:
			case EBML_ID_VOID:
			default:
				if (ebml_read_skip(info->handle, &l))
					return 0;
				break;
		}

		if (!ebml_consume_child(&length, il, l))
			return 0;
	}

	/*
	 * Matroska default ContentCompAlgo is zlib. PSP path only supports
	 * explicit header stripping, algorithm 3.
	 */
	if (!have_algo || algo != 3)
		return 0;

	return len;
}

static int32_t parse_ebml_contentencoding(mkvinfo_t *info,
                                          mkvinfo_track_t *track)
{
	uint64_t len;
	uint64_t length;
	uint64_t l;
	int32_t il;
	uint64_t order = 0;
	uint64_t scope = 1;
	uint64_t type = 0;
	int compression_seen = 0;

	len = length = ebml_read_length(info->handle, &il);
	if (len == EBML_UINT_INVALID)
		return 0;

	len += il;

	while (length > 0) {
		uint32_t id = ebml_read_id(info->handle, &il);
		if (id == EBML_ID_INVALID)
			return 0;

		switch (id) {
			case MATROSKA_ID_CONTENTENCODINGORDER: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					return 0;

				order = num;
				break;
			}

			case MATROSKA_ID_CONTENTENCODINGSCOPE: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					return 0;

				scope = num;
				break;
			}

			case MATROSKA_ID_CONTENTENCODINGTYPE: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					return 0;

				type = num;
				break;
			}

			case MATROSKA_ID_CONTENTCOMPRESSION:
				l = parse_ebml_contentcompression(info, track);
				if (l == 0)
					return 0;

				compression_seen = 1;
				break;

			case EBML_ID_CRC32:
			case EBML_ID_VOID:
			default:
				if (ebml_read_skip(info->handle, &l))
					return 0;
				break;
		}

		if (!ebml_consume_child(&length, il, l))
			return 0;
	}

	if (order != 0)
		return 0;

	if ((scope & 1) == 0)
		return 0;

	/* Type 0 is compression. Type 1 is encryption; reject. */
	if (type != 0)
		return 0;

	if (!compression_seen)
		return 0;

	return len;
}

static int32_t parse_ebml_trackencodings(mkvinfo_t *info,
                                         mkvinfo_track_t *track)
{
	uint64_t len;
	uint64_t length;
	uint64_t l;
	int32_t il;
	int encodings = 0;

	len = length = ebml_read_length(info->handle, &il);
	if (len == EBML_UINT_INVALID)
		return 0;

	len += il;

	while (length > 0) {
		uint32_t id = ebml_read_id(info->handle, &il);
		if (id == EBML_ID_INVALID)
			return 0;

		switch (id) {
			case MATROSKA_ID_CONTENTENCODING:
				encodings++;
				if (encodings > 1)
					return 0;

				l = parse_ebml_contentencoding(info, track);
				if (l == 0)
					return 0;
				break;

			case EBML_ID_CRC32:
			case EBML_ID_VOID:
			default:
				if (ebml_read_skip(info->handle, &l))
					return 0;
				break;
		}

		if (!ebml_consume_child(&length, il, l))
			return 0;
	}

	return len;
}

static int32_t parse_ebml_trackentry(mkvinfo_t *info)
{
	mkvinfo_track_t *track;
	uint64_t len;
	uint64_t length;
	uint64_t l;
	int32_t il;

	len = length = ebml_read_length(info->handle, &il);
	if (len == EBML_UINT_INVALID)
		return 0;

	len += il;

	if (info->total_tracks >= MATROSKA_MAX_TRACKS) {
		if (ebml_skip_payload(info->handle, length))
			return 0;

		return len;
	}

	track = (mkvinfo_track_t *)malloc(sizeof(mkvinfo_track_t));
	if (!track)
		return 0;

	memset(track, 0, sizeof(mkvinfo_track_t));
	track->channels = 1; /* Matroska Audio element defaults */
	track->samplerate = 8000;

	strncpy(track->language, "und", sizeof(track->language) - 1);
	track->flag_enabled = 1;
	track->flag_default = 1;
	track->flag_forced = 0;

	while (length > 0) {
		uint32_t id = ebml_read_id(info->handle, &il);
		if (id == EBML_ID_INVALID)
			goto err_out;

		switch (id) {
			case MATROSKA_ID_TRACKNUMBER: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					goto err_out;

				if (!ebml_uint64_to_int32(num, &track->tracknum))
					goto err_out;

				break;
			}

			case MATROSKA_ID_TRACKTYPE: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					goto err_out;

				if (!ebml_uint64_to_uint32(num, &track->type))
					goto err_out;

				break;
			}

			case MATROSKA_ID_TRACKAUDIO:
				l = parse_ebml_trackaudio(info, track);
				if (l == 0)
					goto err_out;
				break;

			case MATROSKA_ID_TRACKVIDEO:
				l = parse_ebml_trackvideo(info, track);
				if (l == 0)
					goto err_out;
				break;

			case MATROSKA_ID_CODECID: {
				char *codec_id = ebml_read_ascii(info->handle, &l);
				if (codec_id == NULL)
					goto err_out;

				if (!strcmp(codec_id, MKV_A_AAC) ||
				    !strncmp(codec_id, MKV_A_AAC_MPEG4_PREFIX,
				             strlen(MKV_A_AAC_MPEG4_PREFIX)))
					track->audio_type = FOURCC('m','p','4','a');
				else if (!strcmp(codec_id, MKV_A_MP3))
					track->audio_type = FOURCC('m','p','3',' ');
				else if (!strcmp(codec_id, "A_FLAC"))
					track->audio_type = FOURCC('f','L','a','C');
				else if (!strcmp(codec_id, "A_OPUS"))
					track->audio_type = FOURCC('O','p','u','s');
				else if (!strcmp(codec_id, MKV_V_MPEG4_AVC))
					track->video_type = FOURCC('a','v','c','1');
				else if (!strcmp(codec_id, MKV_S_TEXTUTF8))
					track->video_type = FOURCC('t','x','t','u');
				else if (!strcmp(codec_id, MKV_S_TEXTASCII))
					track->video_type = FOURCC('t','x','t','l');
				else if (!strcmp(codec_id, MKV_S_TEXTSSA))
					track->video_type = FOURCC('s','s','a','u');
				else if (!strcmp(codec_id, MKV_S_TEXTASS))
					track->video_type = FOURCC('a','s','s','u');
				else if (!strcmp(codec_id, MKV_S_TEXTWEBVTT))
					track->video_type = FOURCC('v','t','t','u');

				free(codec_id);
				break;
			}

            case 0x56AA: /* CodecDelay, nanoseconds */
            case 0x56BB: { /* SeekPreRoll, nanoseconds */
                uint64_t value = ebml_read_uint(info->handle, &l);
                if (value == EBML_UINT_INVALID) goto err_out;
                if (id == 0x56AA) track->codec_delay_ns = value;
                else track->seek_preroll_ns = value;
                break;
            }

			case MATROSKA_ID_CODECPRIVATE: {
				int32_t size_len;
				uint64_t payload_size;

				payload_size = ebml_read_length(info->handle, &size_len);
				if (payload_size == EBML_UINT_INVALID)
					goto err_out;

				if (payload_size > MKVINFO_MAX_CODEC_PRIVATE)
					goto err_out;

				if (payload_size > SIZE_MAX - MKV_INPUT_PADDING)
					goto err_out;

				l = payload_size + size_len;

				if (track->private_data) {
					free(track->private_data);
					track->private_data = 0;
					track->private_size = 0;
				}

				track->private_data =
					(uint8_t *)malloc((size_t)payload_size + MKV_INPUT_PADDING);
				if (!track->private_data)
					goto err_out;

				if (io_read_data(info->handle,
				                 track->private_data,
				                 payload_size) != payload_size)
					goto err_out;

				memset(track->private_data + payload_size,
				       0,
				       MKV_INPUT_PADDING);

				track->private_size = (uint32_t)payload_size;
				break;
			}

			case MATROSKA_ID_TRACKDEFAULTDURATION: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					goto err_out;

				if (num != 0) {
					track->duration = num;
					track->time_scale = 1000000000ULL;
				}

				break;
			}

			case MATROSKA_ID_TRACKENCODINGS: {
				l = parse_ebml_trackencodings(info, track);
				if (l == 0)
					goto err_out;
				break;
			}
						case MATROSKA_ID_TRACKNAME: {
				char *s = ebml_read_utf8(info->handle, &l);
				if (s == NULL)
					goto err_out;

				strncpy(track->name, s, sizeof(track->name) - 1);
				free(s);
				break;
			}

			case MATROSKA_ID_TRACKLANGUAGE: {
				char *s = ebml_read_ascii(info->handle, &l);
				if (s == NULL)
					goto err_out;

				strncpy(track->language, s, sizeof(track->language) - 1);
				free(s);
				break;
			}

			case MATROSKA_ID_TRACKLANGUAGEIETF: {
				char *s = ebml_read_ascii(info->handle, &l);
				if (s == NULL)
					goto err_out;

				strncpy(track->language_ietf, s, sizeof(track->language_ietf) - 1);
				free(s);
				break;
			}

			case MATROSKA_ID_TRACKFLAGENABLED: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					goto err_out;

				track->flag_enabled = num ? 1 : 0;
				break;
			}

			case MATROSKA_ID_TRACKFLAGDEFAULT: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					goto err_out;

				track->flag_default = num ? 1 : 0;
				break;
			}

			case MATROSKA_ID_TRACKFLAGFORCED: {
				uint64_t num = ebml_read_uint(info->handle, &l);
				if (num == EBML_UINT_INVALID)
					goto err_out;

				track->flag_forced = num ? 1 : 0;
				break;
			}
			case EBML_ID_CRC32:
			case EBML_ID_VOID:
			default:
				if (ebml_read_skip(info->handle, &l))
					goto err_out;
				break;
		}

		if (!ebml_consume_child(&length, il, l))
			goto err_out;
	}

	info->tracks[info->total_tracks++] = track;
	return len;

err_out:
	mkv_free_trackentry(track);
	return 0;
}

static int32_t parse_ebml_tracks(mkvinfo_t *info)
{
	uint64_t length;
	uint64_t l;
	int32_t il;

	length = ebml_read_length(info->handle, NULL);
	if (length == EBML_UINT_INVALID)
		return 1;

	/*
	 * Avoid duplicate/leaked tracks if Tracks is reached once through
	 * SeekHead and again during linear top-level parsing.
	 */
	if (info->total_tracks > 0)
		return ebml_skip_payload(info->handle, length);

	while (length > 0) {
		uint32_t id = ebml_read_id(info->handle, &il);
		if (id == EBML_ID_INVALID)
			return 1;

		switch (id) {
			case MATROSKA_ID_TRACKENTRY:
				l = parse_ebml_trackentry(info);
				if (l == 0)
					return 1;
				break;

			case EBML_ID_CRC32:
			case EBML_ID_VOID:
			default:
				if (ebml_read_skip(info->handle, &l))
					return 1;
				break;
		}

		if (!ebml_consume_child(&length, il, l))
			return 1;
	}

	return 0;
}

static int mkvinfo_add_index(mkvinfo_t *info,
                             uint64_t time,
                             uint64_t track,
                             uint64_t pos)
{
	mkvinfo_index_t *new_indexes;
	int64_t abs_pos;
	int32_t tracknum;
	int64_t file_len;

	if (info == 0)
		return 0;

	if (info->total_indexes >= MKVINFO_MAX_CUES)
		return 1;

	if (time == EBML_UINT_INVALID ||
	    track == EBML_UINT_INVALID ||
	    pos == EBML_UINT_INVALID)
		return 1;

	if (!ebml_uint64_to_int32(track, &tracknum))
		return 1;

	if (info->segment_start < 0)
		return 1;

	if (pos > (uint64_t)INT64_MAX - (uint64_t)info->segment_start)
		return 1;

	abs_pos = info->segment_start + (int64_t)pos;

	file_len = io_get_length64(info->handle);
	if (file_len > 0 && abs_pos >= file_len)
		return 1;

	if (!(info->total_indexes & 31)) {
		new_indexes =
			(mkvinfo_index_t *)realloc(info->indexes,
			                           (info->total_indexes + 32) *
			                           sizeof(mkvinfo_index_t));
		if (new_indexes == 0)
			return 0;

		info->indexes = new_indexes;
	}

	info->indexes[info->total_indexes].tracknum = tracknum;
	info->indexes[info->total_indexes].timecode = time;
	info->indexes[info->total_indexes].filepos = abs_pos;
	info->total_indexes++;

	return 1;
}

static int32_t parse_ebml_cues(mkvinfo_t *info)
{
	uint64_t length0;
	uint64_t length1;
	uint64_t length2;
	uint64_t l0;
	uint64_t l1;
	uint64_t l2;
	uint64_t cue_time;
	uint64_t cue_tracks[MKVINFO_MAX_CUE_TRACK_POSITIONS];
	uint64_t cue_positions[MKVINFO_MAX_CUE_TRACK_POSITIONS];
	int32_t cue_position_count;
	int64_t off;
	int32_t i;
	int32_t i0;
	int32_t i1;
	int32_t i2;
	int32_t il0;
	int32_t il1;
	int64_t end_pos;
	int64_t *new_parsed;

	off = io_get_position64(info->handle);

	for (i = 0; i < info->parsed_cues_num; i++) {
		if (info->parsed_cues[i] == off) {
			if (ebml_read_skip(info->handle, NULL))
				return 1;

			return 0;
		}
	}

	new_parsed =
		(int64_t *)realloc(info->parsed_cues,
		                   (info->parsed_cues_num + 1) * sizeof(*new_parsed));
	if (new_parsed == 0)
		return 1;

	info->parsed_cues = new_parsed;
	info->parsed_cues[info->parsed_cues_num++] = off;

	length0 = ebml_read_length(info->handle, NULL);
	if (length0 == EBML_UINT_INVALID)
		return 1;

	if (!ebml_calc_end64(info->handle, length0, &end_pos))
		return 1;

	while (length0 > 0) {
		uint32_t id0;

		cue_time = EBML_UINT_INVALID;
		cue_position_count = 0;

		id0 = ebml_read_id(info->handle, &i0);
		if (id0 == EBML_ID_INVALID)
			return 1;

		switch (id0) {
			case MATROSKA_ID_POINTENTRY:
				length1 = ebml_read_length(info->handle, &il0);
				if (length1 == EBML_UINT_INVALID)
					return 1;

				l0 = length1 + il0;

				while (length1 > 0) {
					uint32_t id1 = ebml_read_id(info->handle, &i1);
					if (id1 == EBML_ID_INVALID)
						return 1;

					switch (id1) {
						case MATROSKA_ID_CUETIME:
							cue_time = ebml_read_uint(info->handle, &l1);
							if (cue_time == EBML_UINT_INVALID)
								return 1;
							break;

						case MATROSKA_ID_CUETRACKPOSITION: {
							uint64_t track = EBML_UINT_INVALID;
							uint64_t pos = EBML_UINT_INVALID;

							length2 = ebml_read_length(info->handle, &il1);
							if (length2 == EBML_UINT_INVALID)
								return 1;

							l1 = length2 + il1;

							while (length2 > 0) {
								uint32_t id2 = ebml_read_id(info->handle, &i2);
								if (id2 == EBML_ID_INVALID)
									return 1;

								switch (id2) {
									case MATROSKA_ID_CUETRACK:
										track = ebml_read_uint(info->handle, &l2);
										if (track == EBML_UINT_INVALID)
											return 1;
										break;

									case MATROSKA_ID_CUECLUSTERPOSITION:
										pos = ebml_read_uint(info->handle, &l2);
										if (pos == EBML_UINT_INVALID)
											return 1;
										break;

									case MATROSKA_ID_CUEBLOCKNUMBER:
									case EBML_ID_CRC32:
									case EBML_ID_VOID:
									default:
										if (ebml_read_skip(info->handle, &l2))
											return 1;
										break;
								}

								if (!ebml_consume_child(&length2, i2, l2))
									return 1;
							}

							if (track != EBML_UINT_INVALID &&
							    pos != EBML_UINT_INVALID &&
							    cue_position_count < MKVINFO_MAX_CUE_TRACK_POSITIONS) {
								cue_tracks[cue_position_count] = track;
								cue_positions[cue_position_count] = pos;
								cue_position_count++;
							}

							break;
						}

						case EBML_ID_CRC32:
						case EBML_ID_VOID:
						default:
							if (ebml_read_skip(info->handle, &l1))
								return 1;
							break;
					}

					if (!ebml_consume_child(&length1, i1, l1))
						return 1;
				}

				for (i = 0; i < cue_position_count; i++) {
					if (!mkvinfo_add_index(info,
					                       cue_time,
					                       cue_tracks[i],
					                       cue_positions[i]))
						return 1;
				}

				break;

			case EBML_ID_CRC32:
			case EBML_ID_VOID:
			default:
				if (ebml_read_skip(info->handle, &l0))
					return 1;
				break;
		}

		if (!ebml_consume_child(&length0, i0, l0))
			return 1;
	}

	if (io_get_position64(info->handle) != end_pos) {
		if (io_set_position64(info->handle, end_pos) != end_pos)
			return 1;
	}

	return 0;
}

static int32_t parse_ebml_skip_top_level(mkvinfo_t *info)
{
	uint64_t length;

	length = ebml_read_length(info->handle, NULL);
	if (length == EBML_UINT_INVALID)
		return 1;

	return ebml_skip_payload(info->handle, length);
}

static int32_t parse_ebml_tags(mkvinfo_t *info)
{
	return parse_ebml_skip_top_level(info);
}

static void mkvinfo_copy_chapter_title(char *dst, size_t cap,
                                       const char *src)
{
    size_t n;
    if (dst == 0 || cap == 0)
        return;
    dst[0] = 0;
    if (src == 0)
        return;
    n = strlen(src);
    if (n >= cap)
        n = cap - 1U;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void mkvinfo_add_chapter(mkvinfo_t *info, uint64_t start_ns,
                                const char *title, int hidden, int enabled)
{
    int32_t i;
    mkvinfo_chapter_t *chapter;
    uint64_t time_ms;

    if (info == 0 || hidden || !enabled)
        return;
    time_ms = start_ns / 1000000ULL;
    for (i = 0; i < info->total_chapters; ++i) {
        if (info->chapters[i].time_ms == time_ms) {
            if (info->chapters[i].title[0] == 0 && title != 0)
                mkvinfo_copy_chapter_title(info->chapters[i].title,
                                           sizeof(info->chapters[i].title), title);
            return;
        }
    }
    if (info->total_chapters >= MATROSKA_MAX_CHAPTERS)
        return;
    chapter = &info->chapters[info->total_chapters++];
    memset(chapter, 0, sizeof(*chapter));
    chapter->time_ms = time_ms;
    mkvinfo_copy_chapter_title(chapter->title, sizeof(chapter->title), title);
    if (chapter->title[0] == 0)
        snprintf(chapter->title, sizeof(chapter->title), "Chapter %ld",
                 (long)info->total_chapters);
}

static int parse_ebml_chapter_display(mkvinfo_t *info, char *title,
                                      size_t title_cap, uint64_t *used_out)
{
    uint64_t length;
    int32_t size_len;
    int64_t end_pos;

    length = ebml_read_length(info->handle, &size_len);
    if (length == EBML_UINT_INVALID ||
        !ebml_calc_end64(info->handle, length, &end_pos))
        return 0;
    if (used_out) *used_out = length + (uint64_t)size_len;

    while (io_get_position64(info->handle) < end_pos) {
        int32_t id_len = 0;
        uint32_t id = ebml_read_id(info->handle, &id_len);
        uint64_t used = 0;
        if (id == EBML_ID_INVALID)
            break;
        if (id == MATROSKA_ID_CHAPSTRING) {
            char *value = ebml_read_utf8(info->handle, &used);
            if (value != 0) {
                if (title[0] == 0)
                    mkvinfo_copy_chapter_title(title, title_cap, value);
                free(value);
            }
            else break;
        }
        else {
            if (ebml_read_skip(info->handle, &used))
                break;
        }
        if (used == 0 || io_get_position64(info->handle) > end_pos)
            break;
    }
    (void)io_set_position64(info->handle, end_pos);
    return 1;
}

static int parse_ebml_chapter_atom(mkvinfo_t *info, uint64_t *used_out,
                                   unsigned int depth)
{
    uint64_t length;
    int32_t size_len;
    int64_t end_pos;
    uint64_t start_ns = 0;
    int have_start = 0;
    int hidden = 0;
    int enabled = 1;
    char title[MATROSKA_MAX_CHAPTER_TITLE];

    if (depth > 8U)
        return 0;
    length = ebml_read_length(info->handle, &size_len);
    if (length == EBML_UINT_INVALID ||
        !ebml_calc_end64(info->handle, length, &end_pos))
        return 0;
    if (used_out) *used_out = length + (uint64_t)size_len;
    memset(title, 0, sizeof(title));

    while (io_get_position64(info->handle) < end_pos) {
        int32_t id_len = 0;
        uint32_t id = ebml_read_id(info->handle, &id_len);
        uint64_t used = 0;
        if (id == EBML_ID_INVALID)
            break;
        switch (id) {
            case MATROSKA_ID_CHAPTERTIMESTART:
                start_ns = ebml_read_uint(info->handle, &used);
                if (start_ns == EBML_UINT_INVALID) goto done;
                have_start = 1;
                break;
            case MATROSKA_ID_CHAPTERTIMEEND:
                if (ebml_read_uint(info->handle, &used) == EBML_UINT_INVALID)
                    goto done;
                break;
            case MATROSKA_ID_CHAPTERFLAGHIDDEN: {
                uint64_t v = ebml_read_uint(info->handle, &used);
                if (v == EBML_UINT_INVALID) goto done;
                hidden = v != 0;
                break;
            }
            case MATROSKA_ID_CHAPTERFLAGENABLED: {
                uint64_t v = ebml_read_uint(info->handle, &used);
                if (v == EBML_UINT_INVALID) goto done;
                enabled = v != 0;
                break;
            }
            case MATROSKA_ID_CHAPTERDISPLAY:
                if (!parse_ebml_chapter_display(info, title, sizeof(title), &used))
                    goto done;
                break;
            case MATROSKA_ID_CHAPTERATOM:
                if (!parse_ebml_chapter_atom(info, &used, depth + 1U))
                    goto done;
                break;
            default:
                if (ebml_read_skip(info->handle, &used))
                    goto done;
                break;
        }
        if (used == 0 || io_get_position64(info->handle) > end_pos)
            break;
    }

done:
    (void)io_set_position64(info->handle, end_pos);
    if (have_start)
        mkvinfo_add_chapter(info, start_ns, title, hidden, enabled);
    return 1;
}

static int parse_ebml_edition(mkvinfo_t *info, uint64_t *used_out)
{
    uint64_t length;
    int32_t size_len;
    int64_t end_pos;
    int32_t chapter_start;
    int hidden = 0;
    int is_default = 0;

    length = ebml_read_length(info->handle, &size_len);
    if (length == EBML_UINT_INVALID ||
        !ebml_calc_end64(info->handle, length, &end_pos))
        return 0;
    if (used_out) *used_out = length + (uint64_t)size_len;
    chapter_start = info->total_chapters;

    while (io_get_position64(info->handle) < end_pos) {
        int32_t id_len = 0;
        uint32_t id = ebml_read_id(info->handle, &id_len);
        uint64_t used = 0;
        if (id == EBML_ID_INVALID)
            break;
        if (id == MATROSKA_ID_CHAPTERATOM) {
            if (!parse_ebml_chapter_atom(info, &used, 0))
                break;
        }
        else if (id == MATROSKA_ID_EDITIONFLAGHIDDEN) {
            uint64_t value = ebml_read_uint(info->handle, &used);
            if (value == EBML_UINT_INVALID) break;
            hidden = value != 0;
        }
        else if (id == MATROSKA_ID_EDITIONFLAGDEFAULT) {
            uint64_t value = ebml_read_uint(info->handle, &used);
            if (value == EBML_UINT_INVALID) break;
            is_default = value != 0;
        }
        else if (ebml_read_skip(info->handle, &used)) {
            break;
        }
        if (used == 0 || io_get_position64(info->handle) > end_pos)
            break;
    }
    (void)io_set_position64(info->handle, end_pos);

    if (hidden || info->total_chapters == chapter_start) {
        info->total_chapters = chapter_start;
        return 1;
    }
    if (!info->chapter_edition_selected) {
        info->chapter_edition_selected = 1;
        info->chapter_edition_default = is_default;
        return 1;
    }
    if (is_default && !info->chapter_edition_default) {
        int32_t count = info->total_chapters - chapter_start;
        memmove(info->chapters, info->chapters + chapter_start,
                (size_t)count * sizeof(info->chapters[0]));
        info->total_chapters = count;
        info->chapter_edition_default = 1;
        return 1;
    }
    /* Alternate/non-default editions are valid metadata but should not be
     * flattened into one ambiguous seek list. */
    info->total_chapters = chapter_start;
    return 1;
}

static int32_t parse_ebml_chapters(mkvinfo_t *info)
{
    uint64_t length;
    int32_t size_len;
    int64_t end_pos;

    length = ebml_read_length(info->handle, &size_len);
    if (length == EBML_UINT_INVALID ||
        !ebml_calc_end64(info->handle, length, &end_pos))
        return 1;

    while (io_get_position64(info->handle) < end_pos) {
        int32_t id_len = 0;
        uint32_t id = ebml_read_id(info->handle, &id_len);
        uint64_t used = 0;
        if (id == EBML_ID_INVALID)
            break;
        if (id == MATROSKA_ID_EDITIONENTRY) {
            if (!parse_ebml_edition(info, &used))
                break;
        }
        else if (ebml_read_skip(info->handle, &used)) {
            break;
        }
        if (used == 0 || io_get_position64(info->handle) > end_pos)
            break;
    }

    /* Chapters are optional metadata. A damaged chapter tree must not reject
     * otherwise playable Tracks/Cues/Clusters; recover at its bounded end. */
    (void)io_set_position64(info->handle, end_pos);
    return 0;
}

static int32_t parse_ebml_attachedfile(mkvinfo_t *info)
{
	uint64_t len;
	uint64_t length;
	uint64_t l;
	int32_t il;
	char name[MATROSKA_MAX_ATTACHMENT_NAME];
	char mime[MATROSKA_MAX_ATTACHMENT_MIME];
	uint8_t *data = 0;
	uint32_t data_size = 0;
	int is_font = 0;

	len = length = ebml_read_length(info->handle, &il);
	if (len == EBML_UINT_INVALID)
		return 0;

	len += il;

	memset(name, 0, sizeof(name));
	memset(mime, 0, sizeof(mime));

	while (length > 0) {
		uint32_t id = ebml_read_id(info->handle, &il);
		if (id == EBML_ID_INVALID)
			goto err_out;

		switch (id) {
			case MATROSKA_ID_FILENAME: {
				char *s = ebml_read_utf8(info->handle, &l);
				if (s == 0)
					goto err_out;

				strncpy(name, s, sizeof(name) - 1);
				free(s);
				break;
			}

			case MATROSKA_ID_FILEMIMETYPE: {
				char *s = ebml_read_ascii(info->handle, &l);
				if (s == 0)
					goto err_out;

				strncpy(mime, s, sizeof(mime) - 1);
				free(s);
				break;
			}

			case MATROSKA_ID_FILEDATA: {
				int32_t size_len;
				uint64_t payload_size;

				payload_size = ebml_read_length(info->handle, &size_len);
				if (payload_size == EBML_UINT_INVALID)
					goto err_out;

				l = payload_size + size_len;

				is_font = mkvinfo_attachment_is_font(name, mime);

				if (!is_font ||
				    info->total_attachments >= MATROSKA_MAX_ATTACHMENTS ||
				    payload_size == 0 ||
				    payload_size > MATROSKA_MAX_ATTACHMENT_SIZE ||
				    payload_size > 0xffffffffULL) {
					if (ebml_skip_payload(info->handle, payload_size))
						goto err_out;

					break;
				}

				data = (uint8_t *)malloc((size_t)payload_size);
				if (data == 0)
					goto err_out;

				if (io_read_data(info->handle, data, payload_size) != payload_size)
					goto err_out;

				data_size = (uint32_t)payload_size;
				break;
			}

			case MATROSKA_ID_FILEDESCRIPTION:
			case MATROSKA_ID_FILEUID:
			case EBML_ID_CRC32:
			case EBML_ID_VOID:
			default:
				if (ebml_read_skip(info->handle, &l))
					goto err_out;
				break;
		}

		if (!ebml_consume_child(&length, il, l))
			goto err_out;
	}

	is_font = mkvinfo_attachment_is_font(name, mime);

	if (data != 0 && data_size > 0 && is_font &&
	    info->total_attachments < MATROSKA_MAX_ATTACHMENTS) {
		mkvinfo_attachment_t *att =
			&info->attachments[info->total_attachments];

		memset(att, 0, sizeof(*att));
		{
			size_t name_len = strlen(name);
			size_t mime_len = strlen(mime);
			if (name_len >= sizeof(att->name))
				name_len = sizeof(att->name) - 1U;
			if (mime_len >= sizeof(att->mime))
				mime_len = sizeof(att->mime) - 1U;
			memcpy(att->name, name, name_len);
			memcpy(att->mime, mime, mime_len);
			att->name[name_len] = 0;
			att->mime[mime_len] = 0;
		}
		att->data = data;
		att->size = data_size;
		att->is_font = 1;

		info->total_attachments++;
		data = 0;
	}

	if (data != 0)
		free(data);

	return len;

err_out:
	if (data != 0)
		free(data);

	return 0;
}

static int32_t parse_ebml_attachments(mkvinfo_t *info)
{
	uint64_t length;
	uint64_t l;
	int32_t il;

	length = ebml_read_length(info->handle, NULL);
	if (length == EBML_UINT_INVALID)
		return 1;

	while (length > 0) {
		uint32_t id = ebml_read_id(info->handle, &il);
		if (id == EBML_ID_INVALID)
			return 1;

		switch (id) {
			case MATROSKA_ID_ATTACHEDFILE:
				l = parse_ebml_attachedfile(info);
				if (l == 0)
					return 1;
				break;

			case EBML_ID_CRC32:
			case EBML_ID_VOID:
			default:
				if (ebml_read_skip(info->handle, &l))
					return 1;
				break;
		}

		if (!ebml_consume_child(&length, il, l))
			return 1;
	}

	return 0;
}

static int32_t parse_ebml_seekhead(mkvinfo_t *info)
{
	uint64_t length0;
	uint64_t length1;
	uint64_t l0;
	uint64_t l1;
	uint64_t seek_pos;
	uint64_t num;
	uint32_t seek_id;
	int32_t res = 0;
	int64_t off;
	int64_t end_pos;
	int64_t saved_pos;
	int64_t target_pos;
	int32_t i;
	int32_t i0;
	int32_t i1;
	int32_t il0;
	int64_t *new_parsed;

	off = io_get_position64(info->handle);

	for (i = 0; i < info->parsed_seekhead_num; i++) {
		if (info->parsed_seekhead[i] == off) {
			if (ebml_read_skip(info->handle, NULL))
				return 1;

			return 0;
		}
	}

	if (info->parsed_seekhead_num >= MKVINFO_MAX_SEEKHEAD_VISITS) {
		if (ebml_read_skip(info->handle, NULL))
			return 1;

		return 0;
	}

	new_parsed =
		(int64_t *)realloc(info->parsed_seekhead,
		                   (info->parsed_seekhead_num + 1) * sizeof(*new_parsed));
	if (new_parsed == 0)
		return 1;

	info->parsed_seekhead = new_parsed;
	info->parsed_seekhead[info->parsed_seekhead_num++] = off;

	length0 = ebml_read_length(info->handle, NULL);
	if (length0 == EBML_UINT_INVALID)
		return 1;

	if (!ebml_calc_end64(info->handle, length0, &end_pos))
		return 1;

	while (length0 > 0 && !res) {
		uint32_t id0;

		seek_id = 0;
		seek_pos = EBML_UINT_INVALID;

		id0 = ebml_read_id(info->handle, &i0);
		if (id0 == EBML_ID_INVALID)
			return 1;

		switch (id0) {
			case MATROSKA_ID_SEEKENTRY:
				length1 = ebml_read_length(info->handle, &il0);
				if (length1 == EBML_UINT_INVALID)
					return 1;

				l0 = length1 + il0;

				while (length1 > 0) {
					uint32_t id1 = ebml_read_id(info->handle, &i1);
					if (id1 == EBML_ID_INVALID)
						return 1;

					switch (id1) {
						case MATROSKA_ID_SEEKID:
							num = ebml_read_uint(info->handle, &l1);
							if (num != EBML_UINT_INVALID && num <= 0xffffffffULL)
								seek_id = (uint32_t)num;
							break;

						case MATROSKA_ID_SEEKPOSITION:
							seek_pos = ebml_read_uint(info->handle, &l1);
							if (seek_pos == EBML_UINT_INVALID)
								return 1;
							break;

						case EBML_ID_CRC32:
						case EBML_ID_VOID:
						default:
							if (ebml_read_skip(info->handle, &l1))
								return 1;
							break;
					}

					if (!ebml_consume_child(&length1, i1, l1))
						return 1;
				}

				break;

			case EBML_ID_CRC32:
			case EBML_ID_VOID:
			default:
				if (ebml_read_skip(info->handle, &l0))
					return 1;
				break;
		}

		if (!ebml_consume_child(&length0, i0, l0))
			return 1;

		if (seek_id == 0 ||
		    seek_id == MATROSKA_ID_CLUSTER ||
		    seek_pos == EBML_UINT_INVALID)
			continue;

		if (info->segment_start < 0)
			continue;

		if (seek_pos > (uint64_t)INT64_MAX - (uint64_t)info->segment_start)
			continue;

		target_pos = info->segment_start + (int64_t)seek_pos;

		if (io_get_length64(info->handle) > 0 &&
		    target_pos >= io_get_length64(info->handle))
			continue;

		saved_pos = io_get_position64(info->handle);

		if (io_set_position64(info->handle, target_pos) != target_pos) {
			res = 1;
		}
		else {
			int32_t actual_id_len = 0;
			uint32_t actual_id = ebml_read_id(info->handle, &actual_id_len);

			if (actual_id == seek_id) {
				switch (seek_id) {
					case MATROSKA_ID_CUES:
						if (parse_ebml_cues(info))
							res = 1;
						break;

					case MATROSKA_ID_TAGS:
						if (parse_ebml_tags(info))
							res = 1;
						break;

					case MATROSKA_ID_SEEKHEAD:
						if (parse_ebml_seekhead(info))
							res = 1;
						break;

					case MATROSKA_ID_CHAPTERS:
						if (parse_ebml_chapters(info))
							res = 1;
						break;

					default:
						break;
				}
			}
		}

		if (io_set_position64(info->handle, saved_pos) != saved_pos)
			return 1;
	}

	if (io_set_position64(info->handle, end_pos) != end_pos)
		return 1;

	return res;
}

static void parse_ebml_internal(mkvinfo_t *info, int metadata_only)
{
	int32_t version;
	char *str;
	int32_t res = 0;
	int64_t file_len;
	int got_info = 0;
	int got_tracks = 0;

	if (info == 0 || info->handle == 0)
		return;

	str = ebml_read_header(info->handle, &version);
	if (str == NULL)
		return;

	if (strcmp(str, "matroska") != 0) {
		free(str);
		return;
	}

	free(str);

	/*
	 * Keep this permissive enough for modern files whose read version is
	 * higher but whose actually used features are still skipped/rejected
	 * safely by this PSP subset parser.
	 */
	if (version > 4)
		return;

	if (ebml_read_id(info->handle, NULL) != MATROSKA_ID_SEGMENT)
		return;

	/*
	 * Segment size may be unknown. Do not use it as the main loop bound;
	 * stop at first Cluster or EOF.
	 */
	ebml_read_length(info->handle, NULL);

	info->timecode_scale = 1000000;
	info->segment_start = io_get_position64(info->handle);
	info->first_cluster_pos = 0;
	info->has_first_cluster_pos = 0;
	info->first_timecode = 0;
	info->has_first_timecode = 0;

	file_len = io_get_length64(info->handle);

	while (!res) {
		uint32_t id;
		int32_t il = 0;

		if (file_len > 0 && io_get_position64(info->handle) >= file_len)
			break;

		id = ebml_read_id(info->handle, &il);

		switch (id) {
			case MATROSKA_ID_INFO:
				res = parse_ebml_info(info);
				if (!res) got_info = 1;
				break;

			case MATROSKA_ID_TRACKS:
				res = parse_ebml_tracks(info);
				if (!res) got_tracks = 1;
				break;

			case MATROSKA_ID_CUES:
				res = metadata_only ? ebml_read_skip(info->handle, NULL) :
				                      parse_ebml_cues(info);
				break;

			case MATROSKA_ID_TAGS:
				res = metadata_only ? ebml_read_skip(info->handle, NULL) :
				                      parse_ebml_tags(info);
				break;

			case MATROSKA_ID_SEEKHEAD:
				/* Browser metadata never follows SeekHead pointers into Cues,
				 * Chapters, Tags or Attachments. This makes probe cost depend on
				 * header size, not movie duration or cue count. */
				res = metadata_only ? ebml_read_skip(info->handle, NULL) :
				                      parse_ebml_seekhead(info);
				break;

			case MATROSKA_ID_CHAPTERS:
				res = metadata_only ? ebml_read_skip(info->handle, NULL) :
				                      parse_ebml_chapters(info);
				break;

			case MATROSKA_ID_ATTACHMENTS:
				res = metadata_only ? ebml_read_skip(info->handle, NULL) :
				                      parse_ebml_attachments(info);
				break;

			case MATROSKA_ID_CLUSTER: {
				int64_t cluster_element_pos;
				int64_t cluster_payload_pos;
				uint64_t cluster_len;

				if (metadata_only) {
					/* Info/Tracks must precede media data in supported files. Do
					 * not inspect even the first Cluster during a menu probe. */
					res = 1;
					break;
				}

				cluster_element_pos = io_get_position64(info->handle) - il;
				cluster_len = ebml_read_length(info->handle, NULL);
				cluster_payload_pos = io_get_position64(info->handle);

				info->first_cluster_pos = cluster_element_pos;
				info->has_first_cluster_pos = 1;

				if (cluster_len != EBML_UINT_INVALID) {
					int64_t cluster_end;

					if (ebml_calc_end64(info->handle, cluster_len, &cluster_end)) {
						while (io_get_position64(info->handle) < cluster_end) {
							uint32_t child_id;
							int32_t child_id_len;
							uint64_t skipped_len;

							child_id = ebml_read_id(info->handle, &child_id_len);
							if (child_id == EBML_ID_INVALID)
								break;

							if (child_id == MATROSKA_ID_CLUSTERTIMECODE) {
								uint64_t num = ebml_read_uint(info->handle, NULL);

								if (num != EBML_UINT_INVALID) {
									info->first_timecode = num;
									info->has_first_timecode = 1;
								}

								break;
							}

							if (child_id == EBML_ID_CRC32 ||
							    child_id == EBML_ID_VOID ||
							    child_id != MATROSKA_ID_CLUSTERTIMECODE) {
								if (ebml_read_skip(info->handle, &skipped_len))
									break;
							}
						}
					}
				}
				else {
					/*
					 * Unknown-size Cluster: remember where it starts for
					 * forward playback, but do not scan unbounded metadata.
					 */
					info->first_timecode = 0;
					info->has_first_timecode = 1;
				}

				io_set_position64(info->handle, cluster_payload_pos);
				io_set_position64(info->handle, cluster_element_pos);
				res = 1;
				break;
			}

			case EBML_ID_CRC32:
			case EBML_ID_VOID:
				if (ebml_read_skip(info->handle, NULL))
					res = 1;
				break;

			case EBML_ID_INVALID:
				res = 1;
				break;

			default:
				res = metadata_only ? ebml_read_skip(info->handle, NULL) : 1;
				break;
		}

		if (metadata_only && !res && got_info && got_tracks)
			res = 1;
	}

	if (info->parsed_cues) {
		free(info->parsed_cues);
		info->parsed_cues = 0;
		info->parsed_cues_num = 0;
	}

	if (info->parsed_seekhead) {
		free(info->parsed_seekhead);
		info->parsed_seekhead = 0;
		info->parsed_seekhead_num = 0;
	}
}

void parse_ebml(mkvinfo_t *info)
{
	parse_ebml_internal(info, 0);
}

void parse_ebml_metadata(mkvinfo_t *info)
{
	parse_ebml_internal(info, 1);
}
