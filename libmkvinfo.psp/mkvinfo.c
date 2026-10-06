#include "mkvinfo_type.h"
#include "bufferedio.h"
#include "ebml.h"

#include <stdlib.h>
#include <string.h>

static void mkvinfo_sort_chapters(mkvinfo_t *info)
{
	int32_t i;
	if (info == 0) return;
	for (i = 1; i < info->total_chapters; ++i) {
		mkvinfo_chapter_t value = info->chapters[i];
		int32_t j = i;
		while (j > 0 && info->chapters[j - 1].time_ms > value.time_ms) {
			info->chapters[j] = info->chapters[j - 1];
			--j;
		}
		info->chapters[j] = value;
	}
	/* SeekHead recursion can expose the same Chapters element twice. */
	if (info->total_chapters > 1) {
		int32_t out = 1;
		for (i = 1; i < info->total_chapters; ++i) {
			if (info->chapters[i].time_ms == info->chapters[out - 1].time_ms)
				continue;
			if (out != i) info->chapters[out] = info->chapters[i];
			++out;
		}
		info->total_chapters = out;
	}
}

static mkvinfo_t* mkvinfo_open_internal(const char* filename,
                                        int metadata_only)
{
	mkvinfo_t* info;
	buffered_io_t io;
	int32_t result;

	if (filename == 0)
		return 0;

	info = (mkvinfo_t*)malloc(sizeof(mkvinfo_t));
	if (!info)
		return 0;

	memset(info, 0, sizeof(mkvinfo_t));
	info->timecode_scale = 1000000;

	result = io_open64(filename, (void*)(&io));
	if (result < 0) {
		free(info);
		return 0;
	}

	info->handle = &io;
	if (metadata_only)
		parse_ebml_metadata(info);
	else
		parse_ebml(info);
	if (!metadata_only)
		mkvinfo_sort_chapters(info);
	io_close(&io);
	info->handle = 0;

	return info;
}

mkvinfo_t* mkvinfo_open(const char* filename)
{
	return mkvinfo_open_internal(filename, 0);
}

mkvinfo_t* mkvinfo_open_metadata(const char* filename)
{
	return mkvinfo_open_internal(filename, 1);
}

void mkvinfo_close(mkvinfo_t* info)
{
	int32_t i;

	if (info == 0)
		return;

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

	if (info->indexes) {
		free(info->indexes);
		info->indexes = 0;
		info->total_indexes = 0;
	}

	for (i = 0; i < info->total_tracks; i++) {
		if (info->tracks[i]) {
			if (info->tracks[i]->private_data) {
				free(info->tracks[i]->private_data);
				info->tracks[i]->private_data = 0;
			}

			if (info->tracks[i]->compress_setting) {
				free(info->tracks[i]->compress_setting);
				info->tracks[i]->compress_setting = 0;
			}

			free(info->tracks[i]);
			info->tracks[i] = 0;
		}
	}

	info->total_tracks = 0;

	for (i = 0; i < info->total_attachments; i++) {
		if (info->attachments[i].data) {
			free(info->attachments[i].data);
			info->attachments[i].data = 0;
		}
	}

	info->total_attachments = 0;

	free(info);
}
