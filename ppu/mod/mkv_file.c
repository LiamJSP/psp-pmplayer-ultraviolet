/* mkv_file.c
 * Copyright (C) 2009 cooleyes
 *
 * PSP-oriented Matroska open/track-selection hardening.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "mkv_file.h"
#include "../common/ppa_utf8.h"
#include "../common/ppa_video_limits.h"
#include "../common/ppa_time.h"

#define MKV_FOURCC_AVC1 0x61766331U /* avc1 */
#define MKV_FOURCC_MP4A 0x6D703461U /* mp4a */
#define MKV_FOURCC_MP3  0x6D703320U /* mp3  */
#define MKV_FOURCC_TXTU 0x74787475U /* txtu */
#define MKV_FOURCC_TXTL 0x7478746CU /* txtl */
#define MKV_FOURCC_SSAU 0x73736175U /* ssau */
#define MKV_FOURCC_ASSU 0x61737375U /* assu */

#define MKV_DEFAULT_SEEK_DURATION_MS 30000

static int mkv_file_subtitle_track_supported(const mkvinfo_track_t *track);

static uint64_t mkv_file_gcd(uint64_t a, uint64_t b)
{
	while (b) {
		uint64_t remainder = a % b;
		a = b;
		b = remainder;
	}
	return a;
}

static int mkv_file_subtitle_track_already_added(struct mkv_file_struct *p,
                                                 int track_id)
{
	int i;

	for (i = 0; i < p->subtitle_tracks; i++) {
		if (p->subtitle_track_ids[i] == track_id)
			return 1;
	}

	return 0;
}

static void mkv_file_add_subtitle_track_if_supported(struct mkv_file_struct *p,
                                                     int track_id,
                                                     int require_forced,
                                                     int require_default)
{
	mkvinfo_track_t *track;

	if (p == 0 || p->info == 0)
		return;

	if (track_id < 0 || track_id >= p->info->total_tracks)
		return;

	if (p->subtitle_tracks >= MAX_SUBTITLES)
		return;

	if (mkv_file_subtitle_track_already_added(p, track_id))
		return;

	track = p->info->tracks[track_id];

	if (!mkv_file_subtitle_track_supported(track))
		return;

	if (require_forced && !track->flag_forced)
		return;

	if (require_default && !track->flag_default)
		return;

	p->subtitle_track_ids[p->subtitle_tracks] = track_id;
	p->subtitle_track_types[p->subtitle_tracks] = track->video_type;
	p->subtitle_tracks++;
}

static int mkv_file_index_compare(const void *a, const void *b)
{
	const mkvinfo_index_t *ia = (const mkvinfo_index_t *)a;
	const mkvinfo_index_t *ib = (const mkvinfo_index_t *)b;

	if (ia->timecode < ib->timecode)
		return -1;

	if (ia->timecode > ib->timecode)
		return 1;

	if (ia->filepos < ib->filepos)
		return -1;

	if (ia->filepos > ib->filepos)
		return 1;

	if (ia->tracknum < ib->tracknum)
		return -1;

	if (ia->tracknum > ib->tracknum)
		return 1;

	return 0;
}

static void mkv_file_sort_indexes(mkvinfo_t *info)
{
	if (info == 0)
		return;

	if (info->indexes == 0)
		return;

	if (info->total_indexes <= 1)
		return;

	qsort(info->indexes,
	      info->total_indexes,
	      sizeof(mkvinfo_index_t),
	      mkv_file_index_compare);
}

static int mkv_file_validate_avcc(const mkvinfo_track_t *track)
{
	const uint8_t *p;
	uint32_t size;
	uint32_t pos;
	uint32_t sps_count;
	uint32_t pps_count;
	uint32_t i;
	uint32_t nal_length_size;

	if (track == 0)
		return 0;

	if (track->video_type != MKV_FOURCC_AVC1)
		return 1;

	if (track->private_data == 0)
		return 0;

	if (track->private_size < 7)
		return 0;

	p = (const uint8_t *)track->private_data;
	size = (uint32_t)track->private_size;

	/* AVCDecoderConfigurationRecord */
	if (p[0] != 1)
		return 0;

	nal_length_size = (p[4] & 0x03) + 1;
	if (nal_length_size != 1 &&
	    nal_length_size != 2 &&
	    nal_length_size != 4)
		return 0;

	sps_count = p[5] & 0x1F;
	if (sps_count == 0)
		return 0;

	pos = 6;

	for (i = 0; i < sps_count; i++) {
		uint32_t nalsize;

		if (pos + 2 > size)
			return 0;

		nalsize = ((uint32_t)p[pos] << 8) | p[pos + 1];
		pos += 2;

		if (nalsize == 0)
			return 0;

		if (pos + nalsize > size)
			return 0;

		pos += nalsize;
	}

	if (pos + 1 > size)
		return 0;

	pps_count = p[pos++];
	if (pps_count == 0)
		return 0;

	for (i = 0; i < pps_count; i++) {
		uint32_t nalsize;

		if (pos + 2 > size)
			return 0;

		nalsize = ((uint32_t)p[pos] << 8) | p[pos + 1];
		pos += 2;

		if (nalsize == 0)
			return 0;

		if (pos + nalsize > size)
			return 0;

		pos += nalsize;
	}

	return 1;
}

static int mkv_file_video_track_supported(const mkvinfo_track_t *track)
{
	if (track == 0)
		return 0;

	if (track->type != MATROSKA_TRACK_VIDEO)
		return 0;

	if (track->video_type != MKV_FOURCC_AVC1)
		return 0;

	if (track->width < 1 || track->height < 1)
		return 0;

	if (track->width > PPA_VIDEO_MAX_CODED_WIDTH ||
	    track->height > PPA_VIDEO_MAX_CODED_HEIGHT)
		return 0;

	if (!mkv_file_validate_avcc(track))
		return 0;

	{
		uint8_t avc_profile;

		if (track->private_data == 0 || track->private_size < 2)
			return 0;

		avc_profile = ((uint8_t *)track->private_data)[1];

		/*
		 * Preserve the original PMPlayer behavior:
		 * baseline AVC above PSP screen resolution is rejected.
		 */
		if (avc_profile == 0x42 &&
		    (track->width > PPA_VIDEO_LCD_WIDTH ||
		     track->height > PPA_VIDEO_LCD_HEIGHT))
			return 0;
	}

	return 1;
}

static int mkv_file_audio_track_supported(const mkvinfo_track_t *track) {
    struct ppu_audio_format format;
    return ppu_audio_format_mkv(track, &format);
}

static int mkv_file_subtitle_track_supported(const mkvinfo_track_t *track)
{
	if (track == 0)
		return 0;

	if (track->type != MATROSKA_TRACK_SUBTITLE)
		return 0;

	/*
	 * Do not reject on flag_enabled here.
	 *
	 * Matroska defaults FlagEnabled to true when the element is absent.
	 * If parser default initialization is missed or an old metadata path
	 * zero-initializes the new field, checking flag_enabled here rejects
	 * every subtitle track and p->subtitle_tracks becomes 0.
	 *
	 * Disabled tracks can be deprioritized later only after we track whether
	 * FlagEnabled was explicitly present.
	 */

	if (track->video_type != MKV_FOURCC_TXTU &&
	    track->video_type != MKV_FOURCC_TXTL &&
	    track->video_type != MKV_FOURCC_SSAU &&
	    track->video_type != MKV_FOURCC_ASSU &&
	    track->video_type != 0x76747475U) /* vttu */
		return 0;

	return 1;
}

void mkv_file_safe_constructor(struct mkv_file_struct *p)
{
	memset(p, 0, sizeof(struct mkv_file_struct));

	p->info = 0;
	p->video_track_id = -1;
	p->audio_tracks = 0;
	p->subtitle_tracks = 0;
	p->audio_up_sample = 0;
	p->audio_resample_num = 1;
	p->audio_resample_den = 1;
	p->audio_channels = 0;
	p->seek_duration = MKV_DEFAULT_SEEK_DURATION_MS;
	p->duration_ms = 0;
	p->chapter_count = 0;
}

void mkv_file_close(struct mkv_file_struct *p)
{
	if (p->info != 0) {
		mkvinfo_close(p->info);
	}

	mkv_file_safe_constructor(p);
}

char *mkv_file_open(struct mkv_file_struct *p, char *s)
{
	int i;

	mkv_file_safe_constructor(p);

	p->info = mkvinfo_open(s);

	if (p->info != 0) {
		uint32_t ci;
		p->chapter_count = p->info->total_chapters > PPA_MAX_CHAPTERS ?
			PPA_MAX_CHAPTERS : (unsigned int)p->info->total_chapters;
		for (ci = 0; ci < p->chapter_count; ++ci) {
			uint64_t ms = p->info->chapters[ci].time_ms;
			p->chapters[ci].time_ms = ms > 0xffffffffULL ? 0xffffffffU : (uint32_t)ms;
			ppa_utf8_sanitize_copy(p->chapters[ci].title,
				sizeof(p->chapters[ci].title), p->info->chapters[ci].title);
		}
	}

	if (p->info == 0) {
		mkv_file_close(p);
		return "mkv_file_open: can't open file";
	}

	mkv_file_sort_indexes(p->info);

	if (p->info->total_indexes > 1) {
		p->seek_duration = 0;

		for (i = 0; i < p->info->total_indexes - 1; i++) {
			if (p->info->indexes[i].timecode > INT64_MAX ||
			    p->info->indexes[i + 1].timecode > INT64_MAX)
				continue;
			int64_t seek_duration =
				(int64_t)p->info->indexes[i + 1].timecode -
				(int64_t)p->info->indexes[i].timecode;

			if (seek_duration > 0 && seek_duration < 0x7fffffffLL) {
				int64_t seek_ms =
					ppa_mkv_timecode_to_ms(seek_duration,
												p->info->timecode_scale);

				if (seek_ms > p->seek_duration && seek_ms <= INT_MAX)
					p->seek_duration = (int)seek_ms;
			}
		}

		if (p->seek_duration <= 0)
			p->seek_duration = MKV_DEFAULT_SEEK_DURATION_MS;
	}
	else {
		p->seek_duration = MKV_DEFAULT_SEEK_DURATION_MS;
	}

	if (p->info->total_indexes == 0 &&
	    !p->info->has_first_cluster_pos) {
		mkv_file_close(p);
		return "mkv_file_open: can't find first cluster";
	}

	for (i = 0; i < p->info->total_tracks; i++) {
		mkvinfo_track_t *track = p->info->tracks[i];

		if (!mkv_file_video_track_supported(track))
			continue;

		p->video_track_id = i;
		p->video_type = track->video_type;
		break;
	}

	if (p->video_track_id < 0) {
		mkv_file_close(p);
		return "mkv_file_open: can't find supported video track in mkv file";
	}

	for (i = 0; i < p->info->total_tracks; i++) {
		mkvinfo_track_t *track = p->info->tracks[i];

		if (!mkv_file_audio_track_supported(track))
			continue;

		if (p->audio_tracks == 0) {
			p->audio_tracks++;
			p->audio_track_ids[p->audio_tracks - 1] = i;
			p->audio_type = track->audio_type;
			p->audio_channels = track->channels;

		}
		else {
			mkvinfo_track_t *old_track =
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

		if (!ppu_audio_format_mkv(track, &p->audio_formats[p->audio_tracks - 1])) {
            mkv_file_close(p); return "audio: invalid track configuration";
        }
		if (p->audio_tracks == 6)
			break;
	}

	if (p->audio_tracks == 0) {
		mkv_file_close(p);
		return "mkv_file_open: can't find supported audio track in mkv file";
	}

	for (i = 0; i < p->info->total_tracks && p->subtitle_tracks < MAX_SUBTITLES; i++)
		mkv_file_add_subtitle_track_if_supported(p, i, 1, 0);

	for (i = 0; i < p->info->total_tracks && p->subtitle_tracks < MAX_SUBTITLES; i++)
		mkv_file_add_subtitle_track_if_supported(p, i, 0, 1);

	for (i = 0; i < p->info->total_tracks && p->subtitle_tracks < MAX_SUBTITLES; i++)
		mkv_file_add_subtitle_track_if_supported(p, i, 0, 0);

	p->video_width = p->info->tracks[p->video_track_id]->width;
	p->video_height = p->info->tracks[p->video_track_id]->height;
	p->display_width = p->info->tracks[p->video_track_id]->display_width;
	p->display_height = p->info->tracks[p->video_track_id]->display_height;

	if (p->display_width == 0)
		p->display_width = p->video_width;

	if (p->display_height == 0)
		p->display_height = p->video_height;

	{
		uint64_t rate = p->info->tracks[p->video_track_id]->time_scale;
		uint64_t scale = p->info->tracks[p->video_track_id]->duration;
		/* Reduce oversized ratios before narrowing; DefaultDuration is an
		 * unsigned 64-bit nanosecond value, not a uint32 video frame scale. */
		if (rate && scale && (rate > UINT_MAX || scale > UINT_MAX)) {
			uint64_t divisor = mkv_file_gcd(rate, scale);
			rate /= divisor;
			scale /= divisor;
		}
		if (rate > UINT_MAX || scale > UINT_MAX) {
			mkv_file_close(p);
			return "mkv_file_open: unsupported video timebase";
		}
		p->video_rate = (unsigned int)rate;
		p->video_scale = (unsigned int)scale;
	}

	if (p->info->duration > 0) {
		uint64_t duration_ms = p->info->duration;
		if (duration_ms > INT_MAX) {
			mkv_file_close(p);
			return "mkv_file_open: unsupported timestamp range";
		}
		p->duration_ms = (unsigned int)duration_ms;
	}

	if (p->video_rate == 0)
		p->video_rate = 25000;

	if (p->video_scale == 0)
		p->video_scale = 1000;

	if ((uint64_t)p->video_rate > (uint64_t)p->video_scale * 61ULL) {
		mkv_file_close(p);
		return "mkv_file_open: frame rate above experimental 60 FPS limit";
	}

	{
		uint64_t frames = 0;

		if (p->info->duration > 0 &&
		    p->video_rate > 0 &&
		    p->video_scale > 0) {
			frames = ((uint64_t)p->info->duration *
			          (uint64_t)p->video_rate) /
			         ((uint64_t)p->video_scale * 1000ULL);
		}

		if (frames > 0xffffffffULL)
			frames = 0xffffffffULL;

		p->number_of_video_frames = (unsigned int)frames;
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

	return 0;
}
