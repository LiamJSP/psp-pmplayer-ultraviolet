#ifndef __MKVINFO_TYPE_H__
#define __MKVINFO_TYPE_H__

#include <stdint.h>
#include <pspiofilemgr.h>

#define MATROSKA_MAX_TRACKS 32

#define MATROSKA_MAX_ATTACHMENTS 4
#define MATROSKA_MAX_ATTACHMENT_SIZE (2 * 1024 * 1024)
#define MATROSKA_MAX_ATTACHMENT_NAME 128
#define MATROSKA_MAX_ATTACHMENT_MIME 64
#define MATROSKA_MAX_TRACK_LANGUAGE 16
#define MATROSKA_MAX_TRACK_LANGUAGE_IETF 32
#define MATROSKA_MAX_TRACK_NAME 64
#define MATROSKA_MAX_CHAPTERS 128
#define MATROSKA_MAX_CHAPTER_TITLE 192

/* matroska track types */
#define MATROSKA_TRACK_VIDEO    0x01
#define MATROSKA_TRACK_AUDIO    0x02
#define MATROSKA_TRACK_COMPLEX  0x03
#define MATROSKA_TRACK_LOGO     0x10
#define MATROSKA_TRACK_SUBTITLE 0x11
#define MATROSKA_TRACK_CONTROL  0x20

typedef struct {
	char name[MATROSKA_MAX_ATTACHMENT_NAME];
	char mime[MATROSKA_MAX_ATTACHMENT_MIME];
	uint8_t* data;
	uint32_t size;
	uint32_t is_font;
} mkvinfo_attachment_t;

typedef struct {
	uint64_t time_ms;
	char title[MATROSKA_MAX_CHAPTER_TITLE];
} mkvinfo_chapter_t;

typedef struct {
	int32_t tracknum;
	uint32_t type;
	uint32_t video_type;
	uint32_t audio_type;

	uint64_t time_scale;
	uint64_t duration;

	uint32_t width;
	uint32_t height;
	uint32_t display_width;
	uint32_t display_height;

	uint32_t channels;
	uint32_t samplerate;
	uint32_t samplebits;
	uint64_t codec_delay_ns;
	uint64_t seek_preroll_ns;

	uint8_t* compress_setting;
	uint32_t compress_setting_size;

	uint8_t* private_data;
	uint32_t private_size;

	char language[MATROSKA_MAX_TRACK_LANGUAGE];
	char language_ietf[MATROSKA_MAX_TRACK_LANGUAGE_IETF];
	char name[MATROSKA_MAX_TRACK_NAME];

	uint32_t flag_enabled;
	uint32_t flag_default;
	uint32_t flag_forced;

} mkvinfo_track_t;

typedef struct {
	int32_t tracknum;
	uint64_t timecode;

	int64_t filepos;
} mkvinfo_index_t;

typedef struct {
	void* handle;

	int64_t* parsed_cues;
	int32_t parsed_cues_num;
	int64_t* parsed_seekhead;
	int32_t parsed_seekhead_num;

	/* Absolute offsets remain signed 64-bit through metadata and playback. */
	int64_t segment_start;

	/*
	 * First Cluster absolute file position. Used to allow forward playback
	 * when the file has no Cues/index.
	 */
	int64_t first_cluster_pos;
	int32_t has_first_cluster_pos;

	uint64_t timecode_scale;
	uint64_t first_timecode;
	int32_t has_first_timecode;
	uint64_t duration;

	mkvinfo_index_t* indexes;
	int32_t total_indexes;

	mkvinfo_track_t* tracks[MATROSKA_MAX_TRACKS];
	int32_t total_tracks;

	mkvinfo_attachment_t attachments[MATROSKA_MAX_ATTACHMENTS];
	int32_t total_attachments;

	mkvinfo_chapter_t chapters[MATROSKA_MAX_CHAPTERS];
	int32_t total_chapters;
	int32_t chapter_edition_selected;
	int32_t chapter_edition_default;

} mkvinfo_t;

#endif
