#include "codec_prx.h"
#include "../common/ppa_process.h"
/* mkv_play.c
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
#include "../common/ppa_video_limits.h"
#include "../common/ctrl.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <pspdisplay.h>
#include "mkv_play.h"
#include "common/ppa_playback_session.h"
#include "media/PlaybackUi.h"
#include "audio_util.h"
#include "audio_vfpu.h"
#include "../common/ppa_wait.h"
#include "../common/ppa_frame_sink.h"
#include "../common/ppa_hardware_profile.h"
#include "../common/ppa_playback_control.h"
#include "../common/ppa_cache.h"
#include "../common/ppa_thread_policy.h"
#include "../common/ppa_bus_manager.h"
#include "subtitle_font_config.h"
#include "subtitle_preferences.h"
#include "../common/ppa_privileged_bridge.h"
#include "../common/ppa_chapter.h"
#include "../common/ppa_overlay.h"
#include "cpu_clock.h"
#include "gu_font.h"
#include "../media/VideoPipeline.h"

#ifndef PPA_MKV_DIRECT_GU
#define PPA_MKV_DIRECT_GU 1
#endif

/*
 * 6.3.47B live-safe deep-reference trials.
 *
 * Preserve the exact 6.3.37 shallow-pyramid path. Once the decoder positively
 * detects a deep pyramid, automatically select the previously best mode-6
 * policy: fixed cadence, no triplet rotation, and no Sony detail2 remap.
 *
 * Deep modes use a private third immutable staging surface. The staging
 * selector avoids both the surface currently scanned out and the most recently
 * submitted NEXTFRAME surface before writing, preventing intermittent tearing
 * when cadence catch-up places two submissions unusually close together.
 *
 * Modes 1-3 test the highest-probability decoder-side causes while preserving
 * the same presentation path: reference-B demotion, hard context reopen at
 * every IDR, and their combination. Modes 4-5 are whole-frame containment
 * controls. Mode 7 is an intentionally aggressive display-only macroblock
 * quarantine. All decoder policy changes latch only at an IDR so one GOP never
 * mixes incompatible DPB rules.
 */
/* Retain production restoration and the source/Sony dual-DPB translator.
 * Private staging avoids writing the current and pending scanout surfaces. */
#define PPA_H264X_PRESENT_LOCKED_MODE 5U

#define PPA_H264X_MOTION_MODE_DEFAULT 7U
#define PPA_H264X_MOTION_MODE_MAX 7U
#define PPA_H264X_MOTION_MODE_COUNT 8U

#define PPA_H264X_OUTPUT_SOURCE_REMAP_MAIN 3U
#define PPA_H264X_OUTPUT_SOURCE_REMAP_CACHED 4U
#define PPA_H264X_OUTPUT_SOURCE_BURST_MAIN 5U
#define PPA_H264X_OUTPUT_SOURCE_BURST_CACHED 6U

/* Policy 1 restores trusted neighbors; policy 0 is used across discontinuities. */
#define PPA_H264X_RESTORE_POLICY_DEFAULT 1U
#define PPA_H264X_RESTORE_WAIT_TWO_US 24000U
#define PPA_H264X_RESTORE_AUTO_WAIT_MAX_US 30000U
#define PPA_H264X_RESTORE_AV_GUARD_MS 20
#define PPA_H264X_RESTORE_POLL_US 500U
#define PPA_H264X_RESTORE_ACTION_NONE 0U
#define PPA_H264X_RESTORE_ACTION_ROTATE_START 1U
#define PPA_H264X_RESTORE_ACTION_ROTATE_COMPLETE 2U
#define PPA_H264X_RESTORE_ACTION_MIDPOINT 3U
#define PPA_H264X_RESTORE_ACTION_AUTO_MIDPOINT 6U
#define PPA_H264X_RESTORE_ACTION_FINAL_SUPPRESS 7U
#define PPA_H264X_RESTORE_ACTION_DEEP_INTERP 10U

extern void mkv_decode_h264x_output_remap_set(
	struct mkv_decode_struct *p, unsigned int policy);
extern unsigned int mkv_decode_h264x_output_remap_get(
	struct mkv_decode_struct *p);
extern unsigned int mkv_decode_h264x_deep_b_active(void);

extern void mkv_decode_h264x_dual_dpb_mode_set(unsigned int mode);
extern unsigned int mkv_decode_h264x_dual_dpb_mode_get(void);

static unsigned int g_h264x_vblank_cadence_valid;
static unsigned int g_h264x_vblank_cadence_mode;
static unsigned int g_h264x_vblank_cadence_next;

/* 6.3.46A keeps all new presentation state private to this translation unit so
 * no public structure layout changes are required. */
static void *g_h264x_deep_stage_surface;
static void *g_h264x_last_submitted_surface;
static unsigned int g_h264x_deep_stage_index;

static unsigned int mkv_h264x_motion_mode_dual_dpb(unsigned int mode)
{
	return mode == 7U ? 3U : 0U;
}

static unsigned int mkv_h264x_motion_mode_remap_policy(unsigned int mode)
{
	/* Every deep crossroads mode uses the proven no-remap triple-stage path.
	 * Mode 0 alone retains the exact 6.3.37 shallow baseline. */
	return mode == 0U ? 2U : 0U;
}

#define MKV_H264X_PRESENT_ROLE_OTHER             0U
#define MKV_H264X_PRESENT_ROLE_MAIN_NONREF_B     1U
#define MKV_H264X_PRESENT_ROLE_REFERENCE_B       2U
#define MKV_H264X_PRESENT_ROLE_CACHED_NONREF_B   3U
#define MKV_H264X_PRESENT_ROLE_COUNT             4U
#define MKV_H264X_PRESENT_MODE_COUNT             10U
#define MKV_H264X_LOOKAHEAD_FAIL_NONE            0U
#define MKV_H264X_LOOKAHEAD_FAIL_ANCHOR          1U
#define MKV_H264X_LOOKAHEAD_FAIL_QUEUE_TIMEOUT   2U
#define MKV_H264X_LOOKAHEAD_FAIL_NEXT_INVALID    3U
#define MKV_H264X_LOOKAHEAD_FAIL_SEQUENCE        4U
#define MKV_H264X_LOOKAHEAD_FAIL_ROLE            5U
#define MKV_H264X_LOOKAHEAD_FAIL_NEXT_DELTA      6U
#define MKV_H264X_LOOKAHEAD_FAIL_PREVIOUS_DELTA  7U
#define MKV_H264X_LOOKAHEAD_FAIL_GEOMETRY        8U
#define MKV_H264X_LOOKAHEAD_FAIL_SEMA            9U
#define MKV_H264X_LOOKAHEAD_FAIL_COUNT           10U

#ifndef PPA_H264X_PRESENT_LOOKAHEAD_POLL_US
#define PPA_H264X_PRESENT_LOOKAHEAD_POLL_US 250U
#endif

extern int mkv_decode_h264x_reorder_boundary_for_ts(
	struct mkv_decode_struct *p, int display_ts, int frame_duration,
	unsigned int *epoch_out, int *idr_ts_out, int *zero_ts_out,
	unsigned int *zero_au_out, unsigned int *recovery_au_out,
	unsigned int *recovery_pic_num_out);

#ifndef PPA_H264X_SHUTDOWN_WAIT_US
#define PPA_H264X_SHUTDOWN_WAIT_US 1500000U
#endif
#ifndef PPA_H264X_SHUTDOWN_FORCE_WAIT_US
#define PPA_H264X_SHUTDOWN_FORCE_WAIT_US 250000U
#endif

static int mkv_play_lang_code_match(const char *lang,
                                    const char *a,
                                    const char *b,
                                    const char *c);

static const char *mkv_play_language_display_name(const char *lang);

static mkvinfo_track_t *mkv_play_subtitle_track_for_parser_index(struct mkv_play_struct *p,
                                                                 int subtitle_index)
{
	const char *filename;
	const char *prefix = "mkv subtitle track(";
	long parsed_tracknum;
	int32_t tracknum;
	int i;

	if (p == 0)
		return 0;

	if (subtitle_index < 0 ||
	    subtitle_index >= (int)p->subtitle_count)
		return 0;

	if (p->decoder.reader.file.info == 0)
		return 0;

	filename = subtitle_parser[subtitle_index].filename;

	if (strncmp(filename, prefix, strlen(prefix)) != 0)
		return 0;

	parsed_tracknum = -1;
	if (sscanf(filename + strlen(prefix), "%ld", &parsed_tracknum) != 1)
		return 0;

	tracknum = (int32_t)parsed_tracknum;

	for (i = 0; i < p->decoder.reader.file.info->total_tracks; i++) {
		mkvinfo_track_t *track = p->decoder.reader.file.info->tracks[i];

		if (track != 0 && track->tracknum == tracknum)
			return track;
	}

	return 0;
}

static int mkv_play_preferred_subtitle_score(mkvinfo_track_t *track,
                                             const char *preferred_language)
{
	int score = 0;

	if (track == 0)
		return 0;

	if (subtitle_preferences_language_matches(track->language,
	                                          track->language_ietf,
	                                          preferred_language)) {
		score += 300;

		if (track->flag_forced)
			score += 40;

		if (track->flag_default)
			score += 20;

		return score;
	}

	if (track->flag_forced)
		score += 40;

	if (track->flag_default)
		score += 20;

	return score;
}

static int mkv_play_choose_preferred_subtitle(struct mkv_play_struct *p)
{
	const char *preferred_language;
	int i;
	int best_subtitle;
	int best_score;

	if (p == 0 || p->subtitle_count == 0)
		return 0;

	preferred_language = subtitle_preferences_get_preferred_language();

	best_subtitle = 1;
	best_score = -1;

	for (i = 0; i < (int)p->subtitle_count; i++) {
		mkvinfo_track_t *track;
		int score;

		track = mkv_play_subtitle_track_for_parser_index(p, i);
		score = mkv_play_preferred_subtitle_score(track,
		                                          preferred_language);

		if (score > best_score) {
			best_score = score;
			best_subtitle = i + 1;
		}
	}

	return best_subtitle;
}

static int mkv_play_find_track_by_tracknum(struct mkv_play_struct *p,
                                           int32_t tracknum);

static mkvinfo_track_t *mkv_play_get_current_subtitle_track(struct mkv_play_struct *p)
{
	if (p == 0 || p->subtitle == 0 || p->subtitle > p->subtitle_count)
		return 0;

	return mkv_play_subtitle_track_for_parser_index(p, (int)p->subtitle - 1);
}

static void mkv_play_refresh_subtitle_font_set(struct mkv_play_struct *p)
{
	mkvinfo_track_t *track;

	if (p == 0)
		return;

	track = mkv_play_get_current_subtitle_track(p);

	subtitle_font_config_load_for_track(track);

}

static void mkv_play_service_subtitle_font_reload(volatile struct mkv_play_struct *vp)
{
	struct mkv_play_struct *p;

	if (vp == 0)
		return;

	if (!vp->subtitle_font_reload_pending)
		return;

	/*
	 * Only the demux thread may destroy/reload gu_font fallback faces.
	 *
	 * The input thread is allowed to request a reload, but it must not call
	 * gu_font_clear_fallbacks(), FT_Done_Face(), hb_font_destroy(), or cache
	 * reset while the demux thread may be rendering subtitles.
	 */
	vp->subtitle_font_reload_pending = 0;

	p = (struct mkv_play_struct *)vp;

	mkv_play_refresh_subtitle_font_set(p);
}

static void mkv_play_subtitle_label(struct mkv_play_struct *p,
                                    int subtitle_index,
                                    char *out,
                                    int out_size);
static int mkv_play_lang_code_match(const char *lang, const char *a, const char *b, const char *c)
{
	size_t len;
	char tmp[16];
	size_t i;

	if (lang == 0 || lang[0] == '\0')
		return 0;

	memset(tmp, 0, sizeof(tmp));

	len = strlen(lang);
	if (len >= sizeof(tmp))
		len = sizeof(tmp) - 1;

	for (i = 0; i < len; i++) {
		char ch = lang[i];

		if (ch == '-' || ch == '_')
			break;

		if (ch >= 'A' && ch <= 'Z')
			ch = (char)(ch + ('a' - 'A'));

		tmp[i] = ch;
	}

	tmp[i] = '\0';

	if (a != 0 && strcmp(tmp, a) == 0)
		return 1;

	if (b != 0 && strcmp(tmp, b) == 0)
		return 1;

	if (c != 0 && strcmp(tmp, c) == 0)
		return 1;

	return 0;
}

static const char *mkv_play_language_display_name(const char *lang)
{
	if (lang == 0 || lang[0] == '\0')
		return "Unknown";

	if (mkv_play_lang_code_match(lang, "und", 0, 0))
		return "Unknown";

	if (mkv_play_lang_code_match(lang, "en", "eng", 0))
		return "English";

	if (mkv_play_lang_code_match(lang, "es", "spa", 0))
		return "Spanish";

	if (mkv_play_lang_code_match(lang, "fr", "fre", "fra"))
		return "French";

	if (mkv_play_lang_code_match(lang, "de", "ger", "deu"))
		return "German";

	if (mkv_play_lang_code_match(lang, "it", "ita", 0))
		return "Italian";

	if (mkv_play_lang_code_match(lang, "pt", "por", 0))
		return "Portuguese";

	if (mkv_play_lang_code_match(lang, "pl", "pol", 0))
		return "Polish";

	if (mkv_play_lang_code_match(lang, "tr", "tur", 0))
		return "Turkish";

	if (mkv_play_lang_code_match(lang, "sv", "swe", 0))
		return "Swedish";

	if (mkv_play_lang_code_match(lang, "da", "dan", 0))
		return "Danish";

	if (mkv_play_lang_code_match(lang, "fi", "fin", 0))
		return "Finnish";

	if (mkv_play_lang_code_match(lang, "nl", "dut", "nld"))
		return "Dutch";

	if (mkv_play_lang_code_match(lang, "no", "nor", 0))
		return "Norwegian";

	if (mkv_play_lang_code_match(lang, "ru", "rus", 0))
		return "Russian";

	if (mkv_play_lang_code_match(lang, "el", "gre", "ell"))
		return "Greek";

	if (mkv_play_lang_code_match(lang, "he", "heb", 0))
		return "Hebrew";

	if (mkv_play_lang_code_match(lang, "ar", "ara", 0))
		return "Arabic";

	if (mkv_play_lang_code_match(lang, "ro", "rum", "ron"))
		return "Romanian";

	if (mkv_play_lang_code_match(lang, "id", "ind", 0))
		return "Indonesian";

	if (mkv_play_lang_code_match(lang, "vi", "vie", 0))
		return "Vietnamese";

	if (mkv_play_lang_code_match(lang, "th", "tha", 0))
		return "Thai";

	if (mkv_play_lang_code_match(lang, "ko", "kor", 0))
		return "Korean";

	if (mkv_play_lang_code_match(lang, "zh", "chi", "zho"))
		return "Chinese";

	if (mkv_play_lang_code_match(lang, "ja", "jpn", 0))
		return "Japanese";

	return lang;
}

static int mkv_play_find_track_by_tracknum(struct mkv_play_struct *p,
                                           int32_t tracknum)
{
	int i;

	if (p == 0 || p->decoder.reader.file.info == 0)
		return -1;

	for (i = 0; i < p->decoder.reader.file.info->total_tracks; i++) {
		mkvinfo_track_t *track = p->decoder.reader.file.info->tracks[i];

		if (track != 0 && track->tracknum == tracknum)
			return i;
	}

	return -1;
}

static void mkv_play_try_load_embedded_subtitle_font(struct mkv_play_struct *p)
{
	int i;

	if (p == 0)
		return;

	if (p->decoder.reader.file.info == 0)
		return;

	for (i = 0; i < p->decoder.reader.file.info->total_attachments; i++) {
		mkvinfo_attachment_t *att =
			&p->decoder.reader.file.info->attachments[i];

		if (!att->is_font)
			continue;

		if (att->data == 0 || att->size == 0)
			continue;

		if (gu_font_load_memory(att->name[0] ? att->name : "mkv embedded font",
		                        att->data,
		                        att->size) == 0) {
			break;
		}
	}
}

static unsigned int mkv_h264x_present_role(
	volatile const struct mkv_play_struct *p,
	const struct mkv_decode_buffer_struct *video_buffer)
{
	(void)p;
	if (video_buffer == 0 || !video_buffer->h264x_valid)
		return MKV_H264X_PRESENT_ROLE_OTHER;

	if (!video_buffer->h264x_is_b)
		return MKV_H264X_PRESENT_ROLE_OTHER;

	if (video_buffer->h264x_is_reference_b)
		return MKV_H264X_PRESENT_ROLE_REFERENCE_B;

	if (video_buffer->h264x_source_kind == 2U ||
	    video_buffer->h264x_source_kind == PPA_H264X_OUTPUT_SOURCE_REMAP_CACHED ||
	    video_buffer->h264x_source_kind == PPA_H264X_OUTPUT_SOURCE_BURST_MAIN ||
	    video_buffer->h264x_source_kind == PPA_H264X_OUTPUT_SOURCE_BURST_CACHED)
		return MKV_H264X_PRESENT_ROLE_CACHED_NONREF_B;

	/* Remapped output stamps exist only in policies 2/3. Policy 1 is a
	 * detector-only dry run and therefore follows the exact 6.3.33 path. */
	if (video_buffer->h264x_source_kind == PPA_H264X_OUTPUT_SOURCE_REMAP_MAIN &&
	    p != 0 && p->h264x_output_remap_policy >= 2U)
		return MKV_H264X_PRESENT_ROLE_CACHED_NONREF_B;

	/* Unknown/nonzero source kinds stay with the main-path candidate so a
	 * newly observed path cannot silently escape the conservative control. */
	return MKV_H264X_PRESENT_ROLE_MAIN_NONREF_B;
}

static unsigned int mkv_h264x_present_visible_width(
	const volatile struct mkv_play_struct *p);



static int mkv_h264x_same_surface(const void *a, const void *b)
{
	if (a == 0 || b == 0)
		return 0;
	return ppa_cached_cptr(a) == ppa_cached_cptr(b);
}

static void *mkv_h264x_present_stage_frame(volatile struct mkv_play_struct *p,
	const void *source, unsigned int mode,
	unsigned int motion_mode)
{
	void *dst;
	void *scanout;
	void *candidate[3];
	unsigned int candidate_count;
	unsigned int selected;
	unsigned int attempt;
	unsigned int index;
	unsigned int bytes;
	unsigned int pitch;
	unsigned int height;
	int scan_pitch;
	int scan_format;
	int getfb_result;

	if (p == 0 || source == 0)
		return 0;
	bytes = p->h264x_stage_surface_bytes;
	dst = 0;
	scanout = 0;
	candidate_count = 2U;
	candidate[0] = p->h264x_stage_surface[0];
	candidate[1] = p->h264x_stage_surface[1];
	candidate[2] = 0;

	if (motion_mode >= 1U && motion_mode <= 7U && g_h264x_deep_stage_surface) {
		candidate_count = 3U;
		candidate[2] = g_h264x_deep_stage_surface;
	}
	index = candidate_count == 3U ? g_h264x_deep_stage_index % candidate_count :
	                               p->h264x_stage_surface_index % candidate_count;
	scan_pitch = scan_format = 0;
	getfb_result = sceDisplayGetFrameBuf(&scanout, &scan_pitch, &scan_format,
	                                    PSP_DISPLAY_SETBUF_IMMEDIATE);
	if (getfb_result < 0) return 0; /* unknown owner: keep native leased source */
	selected = candidate_count;
	for (attempt = 0U; attempt < candidate_count; ++attempt) {
		unsigned int slot = (index + attempt) % candidate_count;
		if (!candidate[slot] || ppa_frame_surface_equal(candidate[slot], scanout) ||
		    ppa_frame_surface_equal(candidate[slot], g_h264x_last_submitted_surface)) continue;
		selected = slot; dst = candidate[slot]; break;
	}
	/* Never overwrite live/pending scanout merely to avoid a fallback. */
	if (selected == candidate_count) return 0;
	if (candidate_count == 3U) g_h264x_deep_stage_index = (selected + 1U) % candidate_count;
	else p->h264x_stage_surface_index = (selected + 1U) % candidate_count;

	if (dst == 0 || bytes == 0U) {
		return 0;
	}
	pitch = (unsigned int)p->decoder.output_texture_width;
	height = ppa_gu_scanout_storage_height();
	if (pitch == 0U || height > bytes / (pitch * 4U)) return 0;
	if (ppa_gu_copy_frame_blocking(dst, pitch, source, pitch,
	                               ppa_gu_scanout_width(), height) < 0) {
		return 0;
	}
	
	return dst;
}

#define MKV_H264X_FIXED_FRAME_US 33367U
#define MKV_H264X_FIXED_POLL_US 1000U
#define MKV_H264X_FIXED_WAIT_MAX_US 45000U

static void mkv_h264x_fixed_cadence_rebase(
	volatile struct mkv_play_struct *p, unsigned int mode,
	unsigned int now_tick)
{
	if (p == 0)
		return;
	p->h264x_fixed_cadence_valid = 1U;
	p->h264x_fixed_cadence_last_mode = mode;
	p->h264x_fixed_cadence_next_tick =
		now_tick + MKV_H264X_FIXED_FRAME_US;
}

static void mkv_h264x_fixed_cadence_before_display(
	volatile struct mkv_play_struct *p, unsigned int mode,
	int original_avsync_status, int audio_ts, int video_ts)
{
	unsigned int begin_us;
	unsigned int now_us;
	unsigned int sleep_calls = 0U;
	unsigned int elapsed_us;
	int wait_us;

	if (p == 0)
		return;

	now_us = sceKernelGetSystemTimeLow();
	if (!p->h264x_fixed_cadence_valid ||
	    p->h264x_fixed_cadence_last_mode != mode) {
		mkv_h264x_fixed_cadence_rebase(p, mode, now_us);
		return;
	}

	/* Entering a video-master mode while video is already late must never lock
	 * the show thread into permanent real-time playback of stale queued frames.
	 * Catch up immediately, then rebase the 29.97-fps timer. */
	if (original_avsync_status == 2 ||
	    audio_ts - video_ts > 2 * p->decoder.video_frame_duration) {
		mkv_h264x_fixed_cadence_rebase(p, mode, now_us);
		return;
	}

	begin_us = now_us;
	wait_us = (int)(p->h264x_fixed_cadence_next_tick - now_us);

	/* If the target is already stale or implausibly far away, rebase instead
	 * of waiting on a wrapped/corrupted absolute tick. */
	if (wait_us <= -(int)MKV_H264X_FIXED_FRAME_US ||
	    wait_us > (int)MKV_H264X_FIXED_WAIT_MAX_US) {
		mkv_h264x_fixed_cadence_rebase(p, mode, now_us);
		return;
	}

	while (wait_us > 0) {
		unsigned int delay_us =
			(wait_us > (int)MKV_H264X_FIXED_POLL_US) ?
			MKV_H264X_FIXED_POLL_US : (unsigned int)wait_us;

		if (p->return_request != 0 ||
		    p->h264x_playback_mode != mode) {
			p->h264x_fixed_cadence_valid = 0U;
			return;
		}

		sceKernelDelayThread(delay_us);
		sleep_calls++;
		now_us = sceKernelGetSystemTimeLow();
		elapsed_us = now_us - begin_us;
		if (elapsed_us >= MKV_H264X_FIXED_WAIT_MAX_US) {
			mkv_h264x_fixed_cadence_rebase(p, mode, now_us);
			break;
		}
		wait_us = (int)(p->h264x_fixed_cadence_next_tick - now_us);
	}

	elapsed_us = sceKernelGetSystemTimeLow() - begin_us;

	/* Preserve the absolute phase when healthy; if work already consumed more
	 * than one frame, rebase rather than accumulating an unreachable backlog. */
	now_us = sceKernelGetSystemTimeLow();
	if ((int)(now_us - p->h264x_fixed_cadence_next_tick) >
	    (int)MKV_H264X_FIXED_FRAME_US)
		mkv_h264x_fixed_cadence_rebase(p, mode, now_us);
	else
		p->h264x_fixed_cadence_next_tick +=
			MKV_H264X_FIXED_FRAME_US;
}

static void mkv_h264x_vblank_cadence_reset(void)
{
	g_h264x_vblank_cadence_valid = 0U;
	g_h264x_vblank_cadence_mode = 0U;
	g_h264x_vblank_cadence_next = 0U;
}

static void mkv_h264x_vblank_cadence_before_sink(
	volatile struct mkv_play_struct *p,
	unsigned int mode,
	unsigned int will_display)
{
	unsigned int now;
	unsigned int target;
	unsigned int waits;

	if (p == 0 || mode >= PPA_H264X_MOTION_MODE_COUNT)
		return;

	now = sceDisplayGetVcount();
	if (!g_h264x_vblank_cadence_valid ||
	    g_h264x_vblank_cadence_mode != mode) {
		g_h264x_vblank_cadence_valid = 1U;
		g_h264x_vblank_cadence_mode = mode;
		g_h264x_vblank_cadence_next = now + 2U;
	}

	/* ppa_frame_sink_display() already waits one VBlank. For a displayed
	 * sample, stop one VBlank before the target and let the sink land exactly
	 * on it. A suppressed sample has no sink wait, so consume both VBlanks
	 * here. This removes the timer+VBlank beat that produced 1/3/5-VBlank
	 * scanout gaps in the supplied mode-1 trace. */
	if ((int)(now - g_h264x_vblank_cadence_next) > 1) {
		g_h264x_vblank_cadence_next = now + 2U;
	}
	target = g_h264x_vblank_cadence_next - (will_display ? 1U : 0U);
	waits = 0U;
	while ((int)(target - now) > 0) {
		if (p->return_request != 0 ||
		    p->paused == 1 ||
		    p->h264x_playback_mode != mode) {
			mkv_h264x_vblank_cadence_reset();
			return;
		}
		sceDisplayWaitVblankStart();
		waits++;
		now = sceDisplayGetVcount();
	}
	g_h264x_vblank_cadence_next += 2U;
}

static unsigned int mkv_h264x_present_surface_bytes(
	const volatile struct mkv_play_struct *p)
{
	unsigned int pitch;

	if (p == 0)
		return 0U;
	pitch = (unsigned int)p->decoder.output_texture_width;
	if (pitch != 512U && pitch != 768U)
		return 0U;
	/* These are post-render scanout surfaces, including both separated TV
	 * fields, not coded-video planes or LCD-only images. */
	return pitch * ppa_gu_scanout_storage_height() * 4U;
}

static unsigned int mkv_h264x_present_visible_width(
	const volatile struct mkv_play_struct *p)
{
	unsigned int width;
	unsigned int pitch;

	if (p == 0)
		return 0U;
	pitch = (unsigned int)p->decoder.output_texture_width;
	width = ppa_gu_scanout_width();
	if (width == 0U || width > pitch)
		width = pitch;
	return width;
}

static unsigned int mkv_h264x_present_fast_avg_pixel(
	unsigned int a,
	unsigned int b)
{
	/* Rounded per-byte average, including alpha. Both source surfaces use the
	 * same alpha, so this is safe and much cheaper than four scalar channels. */
	return (a & b) +
	       (((a ^ b) & 0xfefefefeU) >> 1) +
	       ((a ^ b) & 0x01010101U);
}







static int mkv_h264x_present_peek_next_reference_b(
	volatile struct mkv_play_struct *p,
	unsigned int current_buffer,
	const struct mkv_decode_buffer_struct *current,
	unsigned int wait_limit_us,
	unsigned int *waited_us_out,
	unsigned int *fail_reason_out,
	struct mkv_decode_buffer_struct **next_out)
{
	SceKernelSemaInfo sema_info;
	unsigned int next_buffer;
	struct mkv_decode_buffer_struct *next;
	unsigned int start_us;
	unsigned int waited_us;
	int delta;
	int frame_duration;

	if (next_out != 0)
		*next_out = 0;
	if (waited_us_out != 0)
		*waited_us_out = 0U;
	if (fail_reason_out != 0)
		*fail_reason_out = MKV_H264X_LOOKAHEAD_FAIL_NONE;
	if (p == 0 || current == 0 || next_out == 0 || waited_us_out == 0 ||
	    fail_reason_out == 0)
		return 0;
	if (p->decoder.number_of_frame_buffers <= 1) {
		*fail_reason_out = MKV_H264X_LOOKAHEAD_FAIL_NEXT_INVALID;
		return 0;
	}

	start_us = sceKernelGetSystemTimeLow();
	waited_us = 0U;
	while (1) {
		memset(&sema_info, 0, sizeof(sema_info));
		sema_info.size = sizeof(sema_info);
		if (sceKernelReferSemaStatus(p->semaphore_can_get_video, &sema_info) < 0) {
			*fail_reason_out = MKV_H264X_LOOKAHEAD_FAIL_SEMA;
			break;
		}

		/* The current token has already been consumed by mkv_wait(). A positive
		 * count therefore proves at least one later ring slot is complete. */
		if (sema_info.currentCount >= 1) {
			next_buffer =
				(current_buffer + 1U) % (unsigned int)p->decoder.number_of_frame_buffers;
			next = (struct mkv_decode_buffer_struct *)
				&p->decoder.output_video_frame_buffers[next_buffer];
			if (next->data == 0 || !next->h264x_valid) {
				*fail_reason_out = MKV_H264X_LOOKAHEAD_FAIL_NEXT_INVALID;
				break;
			}
			if (next->h264x_native_output_seq != current->h264x_native_output_seq + 1U) {
				*fail_reason_out = MKV_H264X_LOOKAHEAD_FAIL_SEQUENCE;
				break;
			}
			if ((next->h264x_source_kind != 1U &&
			     next->h264x_source_kind != PPA_H264X_OUTPUT_SOURCE_REMAP_MAIN) ||
			    !next->h264x_is_b || !next->h264x_is_reference_b) {
				*fail_reason_out = MKV_H264X_LOOKAHEAD_FAIL_ROLE;
				break;
			}

			delta = next->timestamp - current->timestamp;
			frame_duration = p->decoder.video_frame_duration;
			if (frame_duration <= 0)
				frame_duration = 33;
			if (delta < frame_duration - 2 || delta > frame_duration + 2) {
				*fail_reason_out = MKV_H264X_LOOKAHEAD_FAIL_NEXT_DELTA;
				break;
			}

			*waited_us_out = waited_us;
			*next_out = next;
			return 1;
		}

		if (wait_limit_us == 0U || p->return_request != 0)
			break;
		waited_us = sceKernelGetSystemTimeLow() - start_us;
		if (waited_us >= wait_limit_us)
			break;
		{
			unsigned int remaining = wait_limit_us - waited_us;
			unsigned int delay_us = PPA_H264X_PRESENT_LOOKAHEAD_POLL_US;
			if (delay_us > remaining)
				delay_us = remaining;
			if (delay_us == 0U)
				break;
			sceKernelDelayThread(delay_us);
		}
		waited_us = sceKernelGetSystemTimeLow() - start_us;
	}

	*waited_us_out = sceKernelGetSystemTimeLow() - start_us;
	if (*fail_reason_out == MKV_H264X_LOOKAHEAD_FAIL_NONE)
		*fail_reason_out = MKV_H264X_LOOKAHEAD_FAIL_QUEUE_TIMEOUT;
	return 0;
}

/* -------------------------------------------------------------------------
 * H.264X 6.3.35 missing-sample restoration.
 *
 * This layer never modifies Sony's hidden YUV DPB and never changes the
 * validated immutable staging/NEXTFRAME presentation path.  It only selects
 * the RGB source copied into that staging path for the main non-reference-B
 * timestamp that 6.3.33/6.3.34A conservatively suppressed.
 */
static int mkv_h264x_restore_poc_step_ok(
	unsigned int from, unsigned int to, unsigned int expected)
{
	int delta;
	unsigned int modulus;

	delta = (int)to - (int)from;
	if (delta == (int)expected)
		return 1;
	for (modulus = 16U; modulus <= 65536U; modulus <<= 1U) {
		if (delta == (int)expected - (int)modulus)
			return 1;
	}
	return 0;
}

static int mkv_h264x_restore_wait_ready(
	volatile struct mkv_play_struct *p,
	unsigned int required,
	unsigned int expected_policy,
	unsigned int wait_limit_us,
	unsigned int *waited_us_out)
{
	SceKernelSemaInfo sema_info;
	unsigned int begin_us;
	unsigned int waited_us;

	if (waited_us_out != 0)
		*waited_us_out = 0U;
	if (p == 0 || waited_us_out == 0)
		return 0;

	begin_us = sceKernelGetSystemTimeLow();
	waited_us = 0U;
	while (1) {
		memset(&sema_info, 0, sizeof(sema_info));
		sema_info.size = sizeof(sema_info);
		if (sceKernelReferSemaStatus(p->semaphore_can_get_video,
		                             &sema_info) < 0)
			break;
		if ((unsigned int)sema_info.currentCount >= required) {
			*waited_us_out = waited_us;
			return 1;
		}
		if (p->return_request != 0 ||
		    p->h264x_restore_policy != expected_policy)
			break;
		waited_us = sceKernelGetSystemTimeLow() - begin_us;
		if (waited_us >= wait_limit_us)
			break;
		{
			unsigned int delay_us;
			unsigned int remaining;
			remaining = wait_limit_us - waited_us;
			delay_us = PPA_H264X_RESTORE_POLL_US;
			if (delay_us > remaining)
				delay_us = remaining;
			if (delay_us == 0U)
				break;
			sceKernelDelayThread(delay_us);
		}
		waited_us = sceKernelGetSystemTimeLow() - begin_us;
	}
	*waited_us_out = sceKernelGetSystemTimeLow() - begin_us;
	return 0;
}

static int mkv_h264x_restore_peek_triplet(
	volatile struct mkv_play_struct *p,
	unsigned int current_buffer,
	const struct mkv_decode_buffer_struct *current,
	unsigned int policy,
	unsigned int wait_limit_us,
	unsigned int *waited_us_out,
	unsigned int *queue_ready_out,
	struct mkv_decode_buffer_struct **next_ref_out,
	struct mkv_decode_buffer_struct **next_cached_out)
{
	unsigned int next_index;
	unsigned int next2_index;
	struct mkv_decode_buffer_struct *next_ref;
	struct mkv_decode_buffer_struct *next_cached;
	int frame_duration;
	int delta1;
	int delta2;

	if (next_ref_out != 0)
		*next_ref_out = 0;
	if (next_cached_out != 0)
		*next_cached_out = 0;
	if (queue_ready_out != 0)
		*queue_ready_out = 0U;
	if (p == 0 || current == 0 || waited_us_out == 0 ||
	    queue_ready_out == 0 || next_ref_out == 0 || next_cached_out == 0)
		return 0;
	if (p->decoder.number_of_frame_buffers < 3)
		return 0;
	if (!mkv_h264x_restore_wait_ready(p, 2U, policy,
	                                 wait_limit_us, waited_us_out))
		return 0;
	*queue_ready_out = 1U;

	next_index = (current_buffer + 1U) %
		(unsigned int)p->decoder.number_of_frame_buffers;
	next2_index = (current_buffer + 2U) %
		(unsigned int)p->decoder.number_of_frame_buffers;
	next_ref = (struct mkv_decode_buffer_struct *)
		&p->decoder.output_video_frame_buffers[next_index];
	next_cached = (struct mkv_decode_buffer_struct *)
		&p->decoder.output_video_frame_buffers[next2_index];

	if (next_ref->data == 0 || next_cached->data == 0 ||
	    !next_ref->h264x_valid || !next_cached->h264x_valid)
		return 0;
	if (next_ref->h264x_native_output_seq !=
	        current->h264x_native_output_seq + 1U ||
	    next_cached->h264x_native_output_seq !=
	        current->h264x_native_output_seq + 2U)
		return 0;
	if (!next_ref->h264x_is_b || !next_ref->h264x_is_reference_b ||
	    (next_ref->h264x_source_kind != PPA_H264X_OUTPUT_SOURCE_REMAP_MAIN &&
	     next_ref->h264x_source_kind != 1U))
		return 0;
	if (!next_cached->h264x_is_b || next_cached->h264x_is_reference_b ||
	    (next_cached->h264x_source_kind != PPA_H264X_OUTPUT_SOURCE_REMAP_CACHED &&
	     next_cached->h264x_source_kind != 2U))
		return 0;

	frame_duration = p->decoder.video_frame_duration;
	if (frame_duration <= 0)
		frame_duration = 33;
	delta1 = next_ref->timestamp - current->timestamp;
	delta2 = next_cached->timestamp - current->timestamp;
	if (delta1 < frame_duration - 2 || delta1 > frame_duration + 2)
		return 0;
	if (delta2 < (frame_duration * 2) - 3 ||
	    delta2 > (frame_duration * 2) + 3)
		return 0;
	if (!mkv_h264x_restore_poc_step_ok(current->h264x_poc_lsb,
	                                  next_ref->h264x_poc_lsb, 2U) ||
	    !mkv_h264x_restore_poc_step_ok(current->h264x_poc_lsb,
	                                  next_cached->h264x_poc_lsb, 4U))
		return 0;

	*next_ref_out = next_ref;
	*next_cached_out = next_cached;
	return 1;
}

static unsigned int mkv_h264x_restore_wait_budget_us(
	int audio_ts, int video_ts, unsigned int hard_max_us)
{
	int lead_ms;
	unsigned int budget_us;

	lead_ms = video_ts - audio_ts;
	if (lead_ms <= PPA_H264X_RESTORE_AV_GUARD_MS)
		return 0U;
	budget_us = (unsigned int)(lead_ms - PPA_H264X_RESTORE_AV_GUARD_MS) * 1000U;
	if (budget_us > hard_max_us)
		budget_us = hard_max_us;
	return budget_us;
}

static int mkv_h264x_restore_prepare_rotation(
	volatile struct mkv_play_struct *p,
	unsigned int current_buffer,
	struct mkv_decode_buffer_struct *current,
	unsigned int policy,
	unsigned int wait_limit_us,
	void **display_source_out,
	unsigned int *action_out)
{
	struct mkv_decode_buffer_struct *next_ref;
	struct mkv_decode_buffer_struct *next_cached;
	unsigned int waited_us;
	unsigned int queue_ready;
	unsigned int bytes;
	unsigned int pitch;
	unsigned int height;

	if (display_source_out != 0)
		*display_source_out = current ? current->data : 0;
	if (action_out != 0)
		*action_out = PPA_H264X_RESTORE_ACTION_NONE;
	if (p == 0 || current == 0 || display_source_out == 0 ||
	    action_out == 0 || policy != 1U)
		return 0;

	waited_us = 0U;
	queue_ready = 0U;
	if (!mkv_h264x_restore_peek_triplet(p, current_buffer, current,
	                                  policy,
	                                  wait_limit_us,
	                                  &waited_us, &queue_ready,
	                                  &next_ref, &next_cached)) {
		
		
		return 0;
	}

	bytes = p->h264x_restore_surface_bytes;
	if (p->h264x_restore_saved_surface == 0 || bytes == 0U) {
		return 0;
	}
	if (p->h264x_restore_swap_pending) {
		return 0;
	}

	pitch = (unsigned int)p->decoder.output_texture_width;
	height = pitch != 0U ? bytes / (pitch * 4U) : 0U;
	if (ppa_gu_copy_frame_blocking(p->h264x_restore_saved_surface,
	                               pitch, current->data, pitch,
	                               pitch, height) < 0) {
		return 0;
	}
	p->h264x_restore_swap_pending = 1U;
	p->h264x_restore_swap_policy = policy;
	p->h264x_restore_swap_pair_id = ++p->h264x_restore_pair_next_id;
	p->h264x_restore_swap_target_native_seq =
		next_cached->h264x_native_output_seq;
	p->h264x_restore_swap_target_timestamp = next_cached->timestamp;
	p->h264x_restore_swap_target_poc_lsb = next_cached->h264x_poc_lsb;
	*display_source_out = next_cached->data;
	*action_out = PPA_H264X_RESTORE_ACTION_ROTATE_START;

	return 1;
}

static int mkv_h264x_restore_generate_interpolated(
	volatile struct mkv_play_struct *p,
	unsigned int current_buffer,
	struct mkv_decode_buffer_struct *current,
	unsigned int policy,
	unsigned int wait_limit_us,
	void **display_source_out,
	unsigned int *action_out)
{
	struct mkv_decode_buffer_struct *next_ref;
	unsigned int *dst;
	const unsigned int *previous;
	const unsigned int *future;
	unsigned int pitch;
	unsigned int width;
	unsigned int height;
	unsigned int bytes;
	unsigned int waited_us;
	unsigned int fail_reason;
	unsigned int x;
	unsigned int y;
	int frame_duration;
	int previous_delta;

	if (display_source_out != 0)
		*display_source_out = current ? current->data : 0;
	if (action_out != 0)
		*action_out = PPA_H264X_RESTORE_ACTION_NONE;
	if (p == 0 || current == 0 || display_source_out == 0 ||
	    action_out == 0 ||
	    policy != 1U)
		return 0;

	if (!p->h264x_present_anchor_history_valid ||
	    p->h264x_present_anchor_history == 0) {
		return 0;
	}
	bytes = p->h264x_restore_surface_bytes;
	if (p->h264x_restore_generated_surface == 0 || bytes == 0U) {
		return 0;
	}

	frame_duration = p->decoder.video_frame_duration;
	if (frame_duration <= 0)
		frame_duration = 33;
	previous_delta = current->timestamp -
		p->h264x_present_anchor_history_timestamp;
	if (previous_delta < frame_duration - 2 ||
	    previous_delta > frame_duration + 2) {
		return 0;
	}

	waited_us = 0U;
	fail_reason = MKV_H264X_LOOKAHEAD_FAIL_NONE;
	if (!mkv_h264x_present_peek_next_reference_b(p,
	                                            current_buffer,
	                                            current,
	                                            wait_limit_us,
	                                            &waited_us,
	                                            &fail_reason,
	                                            &next_ref)) {
		if (fail_reason == MKV_H264X_LOOKAHEAD_FAIL_QUEUE_TIMEOUT ||
		    fail_reason == MKV_H264X_LOOKAHEAD_FAIL_SEMA) {
		} else {
		}
		
		return 0;
	}

	pitch = (unsigned int)p->decoder.output_texture_width;
	width = mkv_h264x_present_visible_width(p);
	height = ppa_gu_scanout_storage_height();
	if (pitch == 0U || width == 0U || height == 0U ||
	    height > bytes / (pitch * 4U)) {
		return 0;
	}
	if (ppa_gu_copy_frame_blocking(p->h264x_restore_generated_surface,
	                               pitch, current->data, pitch,
	                               pitch, bytes / (pitch * 4U)) < 0) {
		return 0;
	}
	dst = (unsigned int *)p->h264x_restore_generated_surface;
	previous = (const unsigned int *)p->h264x_present_anchor_history;
	future = (const unsigned int *)next_ref->data;

		for (y = 0U; y < height; y += 8U) {
			unsigned int block_h;
			block_h = height - y;
			if (block_h > 8U)
				block_h = 8U;
			for (x = 0U; x < width; x += 8U) {
				unsigned int block_w;
				unsigned int by;
				unsigned int bx;
				block_w = width - x;
				if (block_w > 8U)
					block_w = 8U;
				
				
				for (by = 0U; by < block_h; by++) {
					unsigned int row;
					row = (y + by) * pitch;
					for (bx = 0U; bx < block_w; bx++) {
						unsigned int index;
						unsigned int midpoint;
						unsigned int out;
						index = row + x + bx;
						midpoint = mkv_h264x_present_fast_avg_pixel(
							previous[index], future[index]);
						out = midpoint;
						
						if (dst[index] != out) {
							dst[index] = out;
						}
					}
				}
			}
		}
		*action_out = PPA_H264X_RESTORE_ACTION_MIDPOINT;

	ppa_cache_cpu_wrote_ge_will_read(p->h264x_restore_generated_surface,
	                                 bytes);
	
	*display_source_out = p->h264x_restore_generated_surface;
	
	return 1;
}

static int mkv_h264x_present_capture_anchor_surface(
	volatile struct mkv_play_struct *p,
	const void *surface,
	int timestamp)
{
	unsigned int bytes;
	unsigned int pitch;
	unsigned int height;

	if (p == 0 || surface == 0 || p->h264x_present_anchor_history == 0)
		return 0;
	bytes = mkv_h264x_present_surface_bytes(p);
	if (bytes == 0U || p->h264x_present_anchor_history_bytes < bytes)
		return 0;
	/* Repeat-previous mode intentionally presents the anchor itself at the
	 * next media timestamp. Avoid a same-buffer memcpy and advance only the
	 * trusted timestamp identity. */
	if (surface == p->h264x_present_anchor_history) {
		p->h264x_present_anchor_history_valid = 1U;
		p->h264x_present_anchor_history_timestamp = timestamp;
		return 1;
	}
	pitch = (unsigned int)p->decoder.output_texture_width;
	height = pitch != 0U ? bytes / (pitch * 4U) : 0U;
	if (ppa_gu_copy_frame_blocking(p->h264x_present_anchor_history,
	                               pitch, surface, pitch,
	                               pitch, height) < 0)
		return 0;
	p->h264x_present_anchor_history_valid = 1U;
	p->h264x_present_anchor_history_timestamp = timestamp;
	return 1;
}

void mkv_play_safe_constructor(struct mkv_play_struct *p) {
    p->audio_tail_done = 0;
    memset(&p->seek_request, 0, sizeof(p->seek_request));
    p->seek_request.lock = -1;
	p->persistent_state_ready = 0;
	p->trickplay_frame_counter = 0U;
	p->trickplay_settle_pending = 0U;
	p->seek_audio_target_queued = 0U;
	p->seek_in_flight = 0U;
	p->seek_in_flight_epoch = 0U;
	p->seek_in_flight_target_ms = 0;
	ppa_seek_controller_reset(&p->seek_controller);
	p->audio_reserved = -1;

	p->semaphore_can_get_video   = -1;
	p->semaphore_can_put_video   = -1;
	p->semaphore_can_get_audio   = -1;
	p->semaphore_can_put_audio   = -1;

	p->output_thread = -1;
	p->show_thread   = -1;
	p->demux_thread  = -1;

	p->subtitle_font_reload_pending = 0;
	p->h264x_playback_mode = PPA_H264X_MOTION_MODE_DEFAULT;
	if (p->h264x_playback_mode > PPA_H264X_MOTION_MODE_MAX)
		p->h264x_playback_mode = 0U;
	g_h264x_deep_stage_index = 0U;
	g_h264x_last_submitted_surface = 0;
	mkv_h264x_vblank_cadence_reset();
	p->h264x_output_remap_policy =
		mkv_h264x_motion_mode_remap_policy(p->h264x_playback_mode);
	p->h264x_restore_policy = PPA_H264X_RESTORE_POLICY_DEFAULT;
	p->h264x_restore_saved_surface = 0;
	p->h264x_restore_generated_surface = 0;
	p->h264x_restore_surface_bytes = 0U;
	p->h264x_restore_swap_pending = 0U;
	p->h264x_restore_swap_policy = 0U;
	p->h264x_restore_pair_next_id = 0U;
	p->h264x_restore_swap_pair_id = 0U;
	p->h264x_restore_swap_target_native_seq = 0U;
	p->h264x_restore_swap_target_poc_lsb = 0U;
	p->h264x_restore_swap_target_timestamp = -1;
	p->h264x_present_anchor_history = 0;
	p->h264x_present_anchor_history_bytes = 0U;
	p->h264x_present_anchor_history_valid = 0U;
	p->h264x_present_anchor_history_timestamp = -1;
	p->h264x_stage_surface[0] = 0;
	p->h264x_stage_surface[1] = 0;
	g_h264x_deep_stage_surface = 0;
	g_h264x_last_submitted_surface = 0;
	g_h264x_deep_stage_index = 0U;
	p->h264x_stage_surface_bytes = 0U;
	p->h264x_stage_surface_index = 0U;
	p->h264x_fixed_cadence_valid = 0U;
	p->h264x_fixed_cadence_last_mode = 0U;
	p->h264x_fixed_cadence_next_tick = 0U;

	mkv_decode_safe_constructor(&p->decoder);
}

void mkv_play_close(struct mkv_play_struct *p, int usePos, int pspType) {
	/* Firmware cleanup precedes channel drain and card/stat writes. Workers
	 * have joined, or open failed before any playback worker was started. */
	media_codecs_close(&p->decoder.avc, &p->decoder.audio_decoder);
    ppu_seek_request_destroy(&p->seek_request);
	ppa_video_pipeline_set_trickplay_frame_drop(0);
	ppa_audio_accel_session_end();
	
	if (!(p->audio_reserved < 0)) {
		while (sceAudioGetChannelRestLen(0) > 0)
			sceKernelDelayThread(1000);
		sceAudioChRelease(0);
	}
	ppa_privileged_audio_set_frequency(44100);

	if (!(p->semaphore_can_get_video   < 0)) sceKernelDeleteSema(p->semaphore_can_get_video);
	if (!(p->semaphore_can_put_video   < 0)) sceKernelDeleteSema(p->semaphore_can_put_video);
	if (!(p->semaphore_can_get_audio   < 0)) sceKernelDeleteSema(p->semaphore_can_get_audio);
	if (!(p->semaphore_can_put_audio   < 0)) sceKernelDeleteSema(p->semaphore_can_put_audio);
	
	if (!(p->output_thread < 0)) sceKernelDeleteThread(p->output_thread);
	if (!(p->show_thread   < 0)) sceKernelDeleteThread(p->show_thread);
	if (!(p->demux_thread   < 0)) sceKernelDeleteThread(p->demux_thread);

	(void)usePos;
	movie_stat_active_end(&p->current_timestamp);
	if (p->persistent_state_ready && !ppa_session_audio_only())
		mkv_stat_save(p);

	if (p->h264x_present_anchor_history != 0) {
		free_64(p->h264x_present_anchor_history);
		p->h264x_present_anchor_history = 0;
		p->h264x_present_anchor_history_bytes = 0U;
		p->h264x_present_anchor_history_valid = 0U;
		p->h264x_present_anchor_history_timestamp = -1;
	}
	if (p->h264x_restore_saved_surface != 0) {
		free_64(p->h264x_restore_saved_surface);
		p->h264x_restore_saved_surface = 0;
	}
	if (p->h264x_restore_generated_surface != 0) {
		free_64(p->h264x_restore_generated_surface);
		p->h264x_restore_generated_surface = 0;
	}
	p->h264x_restore_surface_bytes = 0U;
	p->h264x_restore_swap_pending = 0U;
	p->h264x_restore_swap_policy = 0U;
	p->h264x_restore_swap_pair_id = 0U;
	p->h264x_restore_swap_target_native_seq = 0U;
	p->h264x_restore_swap_target_poc_lsb = 0U;
	p->h264x_restore_swap_target_timestamp = -1;

	if (p->h264x_stage_surface[0] != 0) {
		free_64(p->h264x_stage_surface[0]);
		p->h264x_stage_surface[0] = 0;
	}
	if (p->h264x_stage_surface[1] != 0) {
		free_64(p->h264x_stage_surface[1]);
		p->h264x_stage_surface[1] = 0;
	}
	if (g_h264x_deep_stage_surface != 0) {
		free_64(g_h264x_deep_stage_surface);
		g_h264x_deep_stage_surface = 0;
	}
	g_h264x_last_submitted_surface = 0;
	g_h264x_deep_stage_index = 0U;
	p->h264x_stage_surface_bytes = 0U;

	mkv_decode_close(&p->decoder, pspType);

	unsigned int i = 0;
	for (i=0; i<p->subtitle_count; i++)
		subtitle_parse_close( &subtitle_parser[i] );
	
	subtitle_font_config_clear();

	mkv_play_safe_constructor(p);
}

static int mkv_wait(volatile struct mkv_play_struct *p,
                    SceUID s,
                    char *e) {
	return ppa_wait_sema_poll(&p->return_request,
	                              s,
	                              e,
	                              (char **)&p->return_result,
	                              PPA_SEMA_WAIT_TIMEOUT_US);
}

static int mkv_avsync_status(int audio_timestamp, int video_timestamp, int video_frame_duration) {

	// if video ahead of audio, do nothing
	if((int64_t)video_timestamp - audio_timestamp > 2LL * video_frame_duration)
		return 0;

	// if audio ahead of video, skip frame
	if((int64_t)audio_timestamp - video_timestamp > 2LL * video_frame_duration)
		return 2;

	return 1;
}

static void mkv_play_clear_h264x_discontinuity_state(
    volatile struct mkv_play_struct *p)
{
    p->h264x_present_anchor_history_valid = 0U;
    p->h264x_present_anchor_history_timestamp = -1;
    p->h264x_restore_swap_pending = 0U;
    p->h264x_restore_swap_policy = 0U;
    p->h264x_restore_swap_pair_id = 0U;
    p->h264x_restore_swap_target_native_seq = 0U;
    p->h264x_restore_swap_target_poc_lsb = 0U;
    p->h264x_restore_swap_target_timestamp = -1;
    p->h264x_stage_surface_index = 0U;
    p->h264x_fixed_cadence_valid = 0U;
    g_h264x_deep_stage_index = 0U;
    g_h264x_last_submitted_surface = 0;
    mkv_h264x_vblank_cadence_reset();
}

static void mkv_play_apply_h264x_mode(volatile struct mkv_play_struct *p,
                                     unsigned int requested_mode,
                                     int discontinuity_mode)
{
    unsigned int mode = requested_mode;
    unsigned int remap_policy;
    unsigned int dual_dpb_mode;

    if (mode != 0U && mode != PPA_H264X_MOTION_MODE_DEFAULT)
        mode = PPA_H264X_MOTION_MODE_DEFAULT;
    if (ppa_video_pipeline_extreme_battery_saver_enabled())
        mode = 0U;

    if (ppa_video_pipeline_extreme_battery_saver_enabled())
        remap_policy = 0U;
    else if (discontinuity_mode)
        remap_policy = 2U;
    else
        remap_policy = mkv_h264x_motion_mode_remap_policy(mode);

    dual_dpb_mode = (discontinuity_mode ||
                     ppa_video_pipeline_extreme_battery_saver_enabled()) ?
        0U : mkv_h264x_motion_mode_dual_dpb(mode);

    p->h264x_playback_mode = mode;
    p->h264x_output_remap_policy = remap_policy;
    p->h264x_restore_policy = discontinuity_mode ? 0U : 1U;
    if (ppa_video_pipeline_extreme_battery_saver_enabled())
        p->h264x_restore_policy = 0U;

    mkv_decode_h264x_dual_dpb_mode_set(dual_dpb_mode);
    mkv_decode_h264x_output_remap_set(
        (struct mkv_decode_struct *)&p->decoder, remap_policy);
}

static void mkv_play_trickplay_end(volatile struct mkv_play_struct *p)
{
    unsigned int restore_mode = p->h264x_trickplay_saved_mode;
    ppa_video_pipeline_set_trickplay_frame_drop(0);
    p->trickplay_frame_counter = 0U;
    mkv_play_clear_h264x_discontinuity_state(p);
    mkv_play_apply_h264x_mode(p, restore_mode, 0);
}

static int mkv_play_begin_seek(volatile struct mkv_play_struct *p, int target_ms)
{
    unsigned int epoch = p->decoder.output_epoch + 1U;
    if (!epoch) epoch = 1U;
    if (!ppa_session_seek_begin(epoch, target_ms)) {
        p->return_result = "playback: seek timeline unavailable";
        p->return_request = 1;
        return 0;
    }
    p->seek_audio_target_queued = 0U;
    p->seek_in_flight_epoch = epoch;
    p->seek_in_flight_target_ms = target_ms;
    __sync_synchronize();
    p->seek_in_flight = 1U;
    return 1;
}

static int mkv_play_do_seek(volatile struct mkv_play_struct *p)
{
    int pos, origin;
    char *result;
    if (p->seek_in_flight) return 0;
    /* Direction edges/ticks commit immediately; chapter bursts settle first.
     * Requests made during preroll update one latest target, not a backlog. */
    if (!ctrl_pending()) {
        int selected = ppu_seek_request_take(
            (struct ppu_seek_request *)&p->seek_request,
            (uint32_t)(cpu_clock_auto_now_us() / 1000ULL), ctrl_read_cont());
        if (selected >= 0) { p->absolute_seek_ms = selected; p->resume_pos = 0; }
    }
    pos = p->absolute_seek_ms >= 0 ? p->absolute_seek_ms : p->resume_pos;
    if (p->absolute_seek_ms < 0 && p->resume_pos <= 0) return 0;
    if (!p->decoder.reader.file.info || p->decoder.reader.file.info->total_indexes <= 0) {
        p->seek = 0; p->absolute_seek_ms = -1; p->resume_pos = 0;
        return 0;
    }
    origin = p->current_timestamp;
    p->absolute_seek_ms = -1;
    p->resume_pos = 0;
    p->seek = 0;
    p->last_keyframe_pos = origin;
    ppa_playback_control_seek(pos);
    if (!mkv_play_begin_seek(p, pos)) return 1;
    result = mkv_decode_seek((struct mkv_decode_struct *)&p->decoder, pos, origin);
    if (result) {
        p->seek_failures++;
        p->return_result = result;
        p->return_request = 1;
        return 1;
    }
    p->seek_transactions++;
    p->current_timestamp = pos;
    p->trickplay_frame_counter = 0U;
    p->trickplay_settle_pending = 0U;
    mkv_play_apply_h264x_mode(p, p->h264x_playback_mode, 0);
    return 1;
}

static void mkv_play_seek_epoch_arrived(volatile struct mkv_play_struct *p,
                                         unsigned int frame_epoch, int video_ts)
{
    if (p == 0 || !p->seek_in_flight ||
        frame_epoch != p->seek_in_flight_epoch ||
        frame_epoch != p->decoder.output_epoch)
        return;

    /* A failed bounded timeline lock leaves mute/epoch active. The next
     * target frame retries rather than permanently silencing the audio. */
    if (!ppa_session_seek_arrived(p->seek_in_flight_epoch, video_ts)) return;
    p->current_timestamp = video_ts;
    if (p->trickplay_settle_pending) {
        p->trickplay_settle_pending = 0U;
        mkv_play_trickplay_end(p);
        ppa_seek_controller_reset(
            (struct ppa_seek_controller *)&p->seek_controller);
        ppa_playback_control_trickplay_leave(video_ts);
    }
    else if (ppa_seek_controller_active(
                 (const struct ppa_seek_controller *)&p->seek_controller)) {
        mkv_play_trickplay_end(p);
        ppa_seek_controller_reset(
            (struct ppa_seek_controller *)&p->seek_controller);
        ppa_playback_control_trickplay_leave(video_ts);
    }
    else {
        /* Fixed seeks already rebuilt the H.264X shadow/dual DPB during
         * keyframe-to-target preroll. Do not clear it again at arrival. */
        ppa_video_pipeline_set_trickplay_frame_drop(0);
        ppa_session_set_paused(0);
        cpu_clock_auto_boost_for_reason(CPU_CLOCK_BOOST_TRICKPLAY_LEAVE);
    }
    /* Release retained target audio only after the controller/mute state
     * is final. Otherwise the audio thread can wake in the middle and discard
     * the very buffer we held for resumption. */
    __sync_synchronize();
    p->seek_in_flight = 0U;
}

static int mkv_show_thread(SceSize input_length, void *input) {
	volatile struct mkv_play_struct *p = *((void **) input);

	p->current_video_buffer_number = 0;

	int wait;
	int avsync_status = 1;
	struct ppa_frame_lease frame_lease;
	ppa_frame_lease_init(&frame_lease);
	int previous_video_ts = -1;
	unsigned int display_epoch = p->decoder.output_epoch;

	while (p->return_request == 0) {
		if (!ppa_session_pause_point(&p->return_request, &p->paused, PPA_SESSION_WORKER_VIDEO)) break;
		wait = mkv_wait(p,
		                p->semaphore_can_get_video,
		                "mkv_show_thread: sceKernelWaitSema failed on semaphore_can_get_video");

		if (wait == -1) {
			break;
		}
		else if (wait == 1) {
			int audio_ts;
			int video_ts;
			int original_avsync_status;
			int display_setbuf_sync;
			int frame_duration_ms;
			int dynamic_cadence;
			int trickplay_active;
			int intentional_trick_drop;
			int trickplay_signed_level;
			unsigned int trickplay_level;
			unsigned int frame_epoch;
			int seek_epoch_frame;
			int seek_arrival;
			unsigned int battery_saver_active;
			unsigned int current_buffer;
			unsigned int h264x_present_role;
			unsigned int h264x_suppressed;
			unsigned int h264x_mode;
			unsigned int h264x_motion_mode;
			unsigned int h264x_use_fixed_cadence;
			unsigned int h264x_use_vblank_cadence;
			unsigned int h264x_use_stage;
			unsigned int h264x_deep_active;
			unsigned int h264x_restore_policy;
			unsigned int h264x_restore_action;
			unsigned int h264x_restore_pending_match;

			unsigned int h264x_restore_wait_budget;
			void *h264x_restore_source;
			void *h264x_display_data;
			struct mkv_decode_buffer_struct *video_buffer;

			current_buffer = (unsigned int)p->current_video_buffer_number;
			video_buffer =
				(struct mkv_decode_buffer_struct *)&p->decoder.output_video_frame_buffers[current_buffer];
			frame_epoch = video_buffer->epoch;
			if (frame_epoch != p->decoder.output_epoch) {
				unsigned int stale_epoch = video_buffer->epoch;
				(void)stale_epoch;
				p->current_video_buffer_number =
					(p->current_video_buffer_number + 1) %
					p->decoder.number_of_frame_buffers;
				if (sceKernelSignalSema(p->semaphore_can_put_video, 1) < 0) {
					p->return_result =
						"mkv_show_thread: stale video release failed";
					p->return_request = 1;
					break;
				}
				previous_video_ts = -1;
				continue;
			}
			/* Only the display owner resets pacer/history. A producer reset
			 * here used to race an old frame already inside a display fence. */
			if (display_epoch != frame_epoch) {
				display_epoch = frame_epoch;
				previous_video_ts = -1;
				ppa_frame_sink_reset_timing();
				mkv_play_clear_h264x_discontinuity_state(p);
			}
			audio_ts = ppa_session_audio_time_ms(p->current_timestamp);
			video_ts = video_buffer->timestamp;
			frame_duration_ms = ppa_frame_sink_effective_duration_ms(
				previous_video_ts, video_ts, p->decoder.video_frame_duration);
			seek_epoch_frame = p->seek_in_flight &&
				p->seek_in_flight_epoch == frame_epoch;
			seek_arrival = seek_epoch_frame &&
				(int64_t)video_ts + (frame_duration_ms > 0 ? frame_duration_ms : 33) >=
					p->seek_in_flight_target_ms;
			trickplay_active = p->seek_in_flight != 0U ||
				p->trickplay_settle_pending != 0U ||
				ppa_seek_controller_active(
					(const struct ppa_seek_controller *)&p->seek_controller);
			trickplay_signed_level = ppa_seek_controller_signed_level(
				(const struct ppa_seek_controller *)&p->seek_controller);
			trickplay_level = (unsigned int)(trickplay_signed_level < 0 ?
				-trickplay_signed_level : trickplay_signed_level);
			battery_saver_active = (unsigned int)
				ppa_video_pipeline_extreme_battery_saver_enabled();
			intentional_trick_drop = (seek_epoch_frame && !seek_arrival) ||
				p->trickplay_settle_pending != 0U ||
				(trickplay_active &&
				 ppa_seek_controller_frame_drop_allowed(
					(const struct ppa_seek_controller *)&p->seek_controller) &&
				 ppa_video_pipeline_should_drop_trickplay_frame(
					trickplay_level, p->trickplay_frame_counter++));
			if (seek_arrival)
				intentional_trick_drop = 0;
			if (trickplay_active)
				p->current_timestamp = video_ts;

			dynamic_cadence = frame_duration_ms <= 25 ||
				(frame_duration_ms - p->decoder.video_frame_duration > 2) ||
				(p->decoder.video_frame_duration - frame_duration_ms > 2);
			(void)dynamic_cadence;
			original_avsync_status = mkv_avsync_status(audio_ts,
			                                           video_ts,
			                                           frame_duration_ms);
			avsync_status = original_avsync_status;
			display_setbuf_sync = PSP_DISPLAY_SETBUF_IMMEDIATE;
			h264x_mode = PPA_H264X_PRESENT_LOCKED_MODE;
			h264x_motion_mode = p->h264x_playback_mode;
			if (h264x_motion_mode > PPA_H264X_MOTION_MODE_MAX)
				h264x_motion_mode = 0U;
			h264x_deep_active = mkv_decode_h264x_deep_b_active();

			/* Deep-pyramid detection is observational in production. Automatic
			 * mode-6 promotion previously disabled the verified mode-5 restoration
			 * path and suppressed roughly half the pictures on some B-pyramid
			 * streams. Keep it available only behind an explicit build opt-in. */
			
			h264x_motion_mode = p->h264x_playback_mode;
			if (h264x_motion_mode > PPA_H264X_MOTION_MODE_MAX)
				h264x_motion_mode = 0U;
			h264x_present_role = mkv_h264x_present_role(p, video_buffer);
			h264x_suppressed = 0U;
			h264x_restore_policy = p->h264x_restore_policy;
			h264x_restore_action = PPA_H264X_RESTORE_ACTION_NONE;
			h264x_restore_pending_match = 0U;
			h264x_restore_wait_budget = mkv_h264x_restore_wait_budget_us(
				audio_ts, video_ts, PPA_H264X_RESTORE_AUTO_WAIT_MAX_US);
			(void)h264x_restore_wait_budget;
			
			h264x_restore_source = video_buffer->data;

			/* Complete a native triplet rotation even if the user changed policy
			 * after its first picture. The saved surface is associated only with
			 * this exact native sequence/timestamp/POC tuple. */
			if (!trickplay_active && !battery_saver_active &&
			    p->h264x_restore_swap_pending && video_buffer->h264x_valid) {
				if (video_buffer->h264x_native_output_seq ==
				        p->h264x_restore_swap_target_native_seq &&
				    video_buffer->timestamp ==
				        p->h264x_restore_swap_target_timestamp &&
				    video_buffer->h264x_poc_lsb ==
				        p->h264x_restore_swap_target_poc_lsb &&
				    p->h264x_restore_saved_surface != 0) {
					h264x_restore_source = p->h264x_restore_saved_surface;
					h264x_restore_action =
						PPA_H264X_RESTORE_ACTION_ROTATE_COMPLETE;
					/* Attribute the second half to the policy that started the
					 * rotation, even if a later control change is pending. */
					h264x_restore_policy = p->h264x_restore_swap_policy;
					h264x_restore_pending_match = 1U;
				}
				else if (video_buffer->h264x_native_output_seq >
				         p->h264x_restore_swap_target_native_seq) {
					p->h264x_restore_swap_pending = 0U;
					p->h264x_restore_swap_policy = 0U;
					p->h264x_restore_swap_pair_id = 0U;
					p->h264x_restore_swap_target_native_seq = 0U;
					p->h264x_restore_swap_target_poc_lsb = 0U;
					p->h264x_restore_swap_target_timestamp = -1;
				}
			}

			if (trickplay_active)
				avsync_status = intentional_trick_drop ? 2 : 1;

			/* Mode 0 preserves the exact 6.3.37 audio-gated path. Every
			 * compatibility mode 7 uses the stable 6.3.41B 33.367 ms timer.
			 * 6.3.43B intentionally removes VBlank lookahead/interpolation so
			 * this basket changes only rotation fallback and detail2 identity. */
			/* The legacy 33.367 ms H264X timer is safe only for stable <=30 FPS
			 * cadence. High-FPS and VFR samples follow their actual PTS. */
			h264x_use_fixed_cadence =
				(!trickplay_active && !battery_saver_active &&
				 h264x_motion_mode >= 1U && !dynamic_cadence) ? 1U : 0U;
			h264x_use_vblank_cadence = 0U;
			h264x_use_stage =
				(p->decoder.h264x_compat_active &&
				 !ppa_gu_direct_scanout_enabled() &&
				 !trickplay_active && !battery_saver_active) ? 1U : 0U;

			if (h264x_use_fixed_cadence || h264x_use_vblank_cadence)
				avsync_status = 1;

			if (!trickplay_active && !battery_saver_active &&
			    (h264x_use_fixed_cadence ||
			     h264x_use_vblank_cadence ||
			     original_avsync_status == 1) &&
			    video_buffer->h264x_valid) {
				if (h264x_mode == 5U &&
				    !h264x_restore_pending_match &&
				    h264x_present_role == MKV_H264X_PRESENT_ROLE_MAIN_NONREF_B) {
					int restored;
					restored = 0;

					/* Deep interpolation is disabled in 6.3.43B. The previous
					 * lookahead modes could deadlock their own future-frame source. */
					 {
						unsigned int allow_rotation;
						allow_rotation =
							(h264x_motion_mode == 0U) ? 1U : 0U;

						if (allow_rotation &&
						    h264x_restore_policy == 1U) {
							unsigned int rotate_wait;
							unsigned int auto_begin_us;
							unsigned int auto_elapsed_us;
							unsigned int midpoint_wait;
							auto_begin_us = sceKernelGetSystemTimeLow();
							rotate_wait = h264x_restore_wait_budget;
							if (rotate_wait > PPA_H264X_RESTORE_WAIT_TWO_US)
								rotate_wait = PPA_H264X_RESTORE_WAIT_TWO_US;
							restored = mkv_h264x_restore_prepare_rotation(
								p, current_buffer, video_buffer,
								h264x_restore_policy, rotate_wait,
								&h264x_restore_source,
								&h264x_restore_action);
							if (!restored) {
								
								auto_elapsed_us =
									sceKernelGetSystemTimeLow() - auto_begin_us;
								midpoint_wait =
									h264x_restore_wait_budget > auto_elapsed_us ?
									h264x_restore_wait_budget - auto_elapsed_us : 0U;
								restored =
									mkv_h264x_restore_generate_interpolated(
										p, current_buffer, video_buffer, 1U,
										midpoint_wait,
										&h264x_restore_source,
										&h264x_restore_action);
								if (restored) {
									h264x_restore_action =
										PPA_H264X_RESTORE_ACTION_AUTO_MIDPOINT;
								}
							}
						}
						else if (!allow_rotation &&
						         h264x_restore_policy == 1U) {
							restored =
								mkv_h264x_restore_generate_interpolated(
									p, current_buffer, video_buffer, 1U,
									h264x_restore_wait_budget,
									&h264x_restore_source,
									&h264x_restore_action);
							if (restored) {
								h264x_restore_action =
									PPA_H264X_RESTORE_ACTION_AUTO_MIDPOINT;
							}
						}

					}

					if (!restored) {
						h264x_suppressed = 1U;
						h264x_restore_action =
							PPA_H264X_RESTORE_ACTION_FINAL_SUPPRESS;
					}
					
				}

			}

			/* The VBlank-grid modes use the sink's built-in VBlank wait as
			 * their second 59.94-Hz tick, so IMMEDIATE is correct there.
			 * Legacy modes retain 6.3.37 NEXTFRAME behavior unchanged. */
			if (h264x_mode == 5U && !h264x_use_vblank_cadence &&
			    !dynamic_cadence)
				display_setbuf_sync = PSP_DISPLAY_SETBUF_NEXTFRAME;

			if (h264x_suppressed)
				avsync_status = 2;

			/* Reserve exactly one timer slot for every consumed valid video sample.
			 * 6.3.41A waited separately in display and suppression branches; in
			 * practice many deep-B suppressed samples bypassed the timer, allowing
			 * demux to run roughly one B-run ahead and fill the compressed audio
			 * queue. Centralizing the wait here makes display, repeat, native
			 * fallback, and final suppression share one monotonic cadence. */
			if (h264x_use_fixed_cadence && video_buffer->h264x_valid &&
			    avsync_status > 0)
				mkv_h264x_fixed_cadence_before_display(
					p, h264x_motion_mode, original_avsync_status,
					audio_ts, video_ts);

			if (seek_arrival) {
				avsync_status = 1;
				/* Ownership fences can display preroll frames and establish an
				 * earlier wall-clock anchor. Normal playback starts a new timeline
				 * here, with target audio still retained until this swap completes. */
				ppa_frame_sink_reset_timing();
			}

			if (avsync_status > 0) {
				int force_display_for_ownership;

				/* Direct scanout (and the legacy main-RAM display path) must keep
				 * the currently scanned framebuffer unavailable to the producer
				 * until a later successful vertical swap retires it. H264X can
				 * intentionally suppress B pictures, so consuming a queue entry is
				 * not itself proof that the previous scanout surface is reusable.
				 *
				 * Accumulate FIFO release credits across suppressed/late frames and
				 * publish them only after a successful display transaction. If the
				 * deferred credits would consume the complete framebuffer ring,
				 * force this picture through the normal sink once to advance scanout
				 * and prevent producer/consumer deadlock. This mirrors MP4's proven
				 * ownership fence and is especially important for the three-surface
				 * eDRAM direct-scanout ring. */
				force_display_for_ownership =
					(avsync_status != 1 &&
					 frame_lease.retained_credits + 1U >=
					     (unsigned int)p->decoder.number_of_frame_buffers);

				if (avsync_status == 1 || force_display_for_ownership) {

					h264x_display_data = h264x_restore_source;
					if (h264x_use_stage) {
						void *staged = mkv_h264x_present_stage_frame(
							p, h264x_restore_source, h264x_mode,
							h264x_motion_mode);
						if (staged != 0)
							h264x_display_data = staged;
					}
					/* A generated/saved workspace or lookahead slot is not the
					 * current FIFO lease. Without an immutable stage, scanning it
					 * would let a later restoration write/credit overwrite live
					 * pixels. Decline this optional RGB restoration and display the
					 * owned native slot; retire any rotation only after the fence. */
					if (ppa_frame_surface_equal(h264x_display_data, h264x_restore_source) &&
					    !ppa_frame_surface_equal(h264x_restore_source, video_buffer->data)) {
						h264x_restore_source = video_buffer->data;
						h264x_display_data = video_buffer->data;
						h264x_restore_action = PPA_H264X_RESTORE_ACTION_NONE;
						if (p->h264x_restore_swap_pending)
							h264x_restore_pending_match = 1U;
					}
					if (h264x_use_vblank_cadence)
						mkv_h264x_vblank_cadence_before_sink(
							p, h264x_motion_mode, 1U);
					if (ppa_frame_sink_display_timed(
						h264x_display_data,
						p->decoder.output_texture_width,
						PSP_DISPLAY_PIXEL_FORMAT_8888,
						display_setbuf_sync,
						video_ts, frame_duration_ms) < 0) {
						p->return_result = "mkv_show_thread: display transaction failed";
						p->return_request = 1;
						break;
					}
					if (!trickplay_active && !p->seek_in_flight &&
					    frame_epoch == p->decoder.output_epoch)
						cpu_clock_auto_on_present_complete(
						    ppa_session_audio_time_ms(p->current_timestamp), video_ts,
						    frame_duration_ms);
					/* A stop during the display fence leaves scanout ownership
					 * with the sink until joined-thread renderer teardown. */
					if (p->return_request != 0)
						break;
					if (seek_arrival)
						mkv_play_seek_epoch_arrived(p, frame_epoch, video_ts);
					if (h264x_motion_mode >= 1U &&
					    h264x_motion_mode <= 7U)
						g_h264x_last_submitted_surface =
							h264x_display_data;
					
					/* History always records the actual pre-overlay RGB source supplied
					 * to immutable staging, never Sony's rejected surface by accident. */
					if (p->decoder.h264x_compat_active &&
					    !trickplay_active && !battery_saver_active &&
					    h264x_restore_action !=
					    PPA_H264X_RESTORE_ACTION_DEEP_INTERP)
						mkv_h264x_present_capture_anchor_surface(
							p, h264x_restore_source, video_ts);
					
					if (h264x_restore_pending_match) {
						unsigned int completed_policy;
						completed_policy = p->h264x_restore_swap_policy;
						p->h264x_restore_swap_pending = 0U;
						p->h264x_restore_swap_policy = 0U;
						p->h264x_restore_swap_pair_id = 0U;
						p->h264x_restore_swap_target_native_seq = 0U;
						p->h264x_restore_swap_target_poc_lsb = 0U;
						p->h264x_restore_swap_target_timestamp = -1;
					}

					/* Only a completed vertical swap proves that the old scanout
					 * surface is retired. Release every FIFO slot consumed since the
					 * previous successful swap, then retain one credit for the surface
					 * that has just become active scanout. */
					int release_result = ppa_frame_lease_complete_swap(&frame_lease,
					    p->semaphore_can_put_video, h264x_display_data,
					    frame_epoch, video_ts);
					if (release_result < 0) {
						p->return_result =
							"mkv_show_thread: display ownership release failed";
						p->return_request = 1;
						break;
					}

				}
				else {
					if (h264x_use_vblank_cadence)
						mkv_h264x_vblank_cadence_before_sink(
							p, h264x_motion_mode, 0U);
					/* Intentional scrub discard is not a missed playback target. */
					if (!intentional_trick_drop) {
						cpu_clock_auto_on_frame_skipped();
					}

					if (h264x_suppressed)
						;
					else {
					}
					
					if (h264x_restore_pending_match) {
						unsigned int failed_policy;
						failed_policy = p->h264x_restore_swap_policy;
						p->h264x_restore_swap_pending = 0U;
						p->h264x_restore_swap_policy = 0U;
						p->h264x_restore_swap_pair_id = 0U;
						p->h264x_restore_swap_target_native_seq = 0U;
						p->h264x_restore_swap_target_poc_lsb = 0U;
						p->h264x_restore_swap_target_timestamp = -1;
					}

					/* The consumed slot is not reusable yet: no display swap occurred,
					 * so the producer's next FIFO slot may still be the active scanout. */
					ppa_frame_lease_skip(&frame_lease);
				}

				previous_video_ts = video_ts;
				p->current_video_buffer_number =
					(p->current_video_buffer_number + 1) %
					p->decoder.number_of_frame_buffers;
			}
			else {

				sceKernelDelayThread(PPA_SEMA_IDLE_DELAY_US);

				if (sceKernelSignalSema(p->semaphore_can_get_video, 1) < 0) {
					p->return_result  = "mkv_show_thread: sceKernelSignalSema failed on semaphore_can_get_video1";
					p->return_request = 1;
					break;
				}

			}
		}
		else {
			sceKernelDelayThread(PPA_SEMA_IDLE_DELAY_US);
		}

		ppa_session_pause_point(&p->return_request, &p->paused, PPA_SESSION_WORKER_VIDEO);
	}

	return(0);
}

static unsigned int FONTCOLORS[] = { 
	0xffffff,
	0xff0000,
	0x00ff00,
	0x0000ff,
	0xffff00,
	0x00ffff
};

static unsigned int BORDERCOLORS[] = {	
	0x000000,
	0x7f0000,
	0x007f00,
	0x00007f,
	0x7f7f00,
	0x007f7f
};

static void mkv_play_toggle_subtitle_track_from_input(volatile struct mkv_play_struct *p)
{
	if (p == 0)
		return;

	if (p->subtitle_count == 0)
		return;

	p->subtitle = (p->subtitle + 1) % (p->subtitle_count + 1);

	ppa_playback_control_subtitle_change(p->subtitle);

	/*
	 * Do not reload fonts directly in the input thread.
	 * The demux thread owns subtitle font reloads.
	 */
	p->subtitle_font_reload_pending = 1;

	if (p->subtitle) {
		char subtitle_label[256];

		mkv_play_subtitle_label((struct mkv_play_struct *)p,
		                        p->subtitle - 1,
		                        subtitle_label,
		                        sizeof(subtitle_label));

		ppa_overlay_postf(PPA_OVERLAY_KEY_SUBTITLE, PPA_OVERLAY_TOP_RIGHT, 1000,
		                  PPA_OVERLAY_UTF8, "[i]%s[/i]", subtitle_label);
	}
	else {
		ppa_overlay_post(PPA_OVERLAY_KEY_SUBTITLE, PPA_OVERLAY_TOP_RIGHT, 1000,
		                 PPA_OVERLAY_UTF8, "[i]no subtitle[/i]");
	}

}

static void mkv_post_seek_label(volatile struct mkv_play_struct *p, int level, int target_ms)
{
    char seek_label[48];
    ppa_seek_controller_format_label(
        seek_label, sizeof(seek_label), level,
        p->decoder.reader.file.duration_ms);
    ppa_overlay_postf(PPA_OVERLAY_KEY_SEEK, PPA_OVERLAY_TOP_RIGHT, 1500,
                      PPA_OVERLAY_UTF8, "[i]%s %s | %02d:%02d:%02d[/i]",
                      level > 0 ? ppa_playback_ui_text("seek.forward", "Seek forward") :
                                  ppa_playback_ui_text("seek.back", "Seek back"), seek_label,
                      target_ms / 3600000, (target_ms / 60000) % 60,
                      (target_ms / 1000) % 60);
}

static int mkv_chapter_skip(volatile struct mkv_play_struct *p, int direction)
{
    const struct mkv_file_struct *file = (const struct mkv_file_struct *)&p->decoder.reader.file;
    int index;
    uint64_t now;
    int base_ms;
    if (file->chapter_count == 0) {
        ppa_overlay_post(PPA_OVERLAY_KEY_CHAPTER, PPA_OVERLAY_CENTER, 3000,
                         PPA_OVERLAY_UTF8 | PPA_OVERLAY_WHITE, ppa_playback_ui_text("chapter.none", "No chapters"));
        return 1;
    }
    if (file->info == 0 || file->info->total_indexes <= 0) {
        ppa_overlay_post(PPA_OVERLAY_KEY_CHAPTER, PPA_OVERLAY_CENTER, 3000,
                         PPA_OVERLAY_UTF8 | PPA_OVERLAY_WHITE,
                         ppa_playback_ui_text("chapter.no_cues", "Chapter seek unavailable: no cue index"));
        return 1;
    }
    now = cpu_clock_auto_now_us();
    base_ms = ppu_seek_request_cursor((struct ppu_seek_request *)&p->seek_request,
        p->current_timestamp, p->seek_in_flight != 0U, (uint32_t)(now / 1000ULL));
    if (p->chapter_skip_cursor >= 0 &&
        now < p->chapter_skip_deadline_us) {
        index = p->chapter_skip_cursor + (direction < 0 ? -1 : 1);
        if (index < 0 || (unsigned int)index >= file->chapter_count)
            index = -1;
    }
    else {
        index = direction < 0 ?
            ppa_chapter_previous(file->chapters, file->chapter_count,
                                 (uint32_t)base_ms) :
            ppa_chapter_next(file->chapters, file->chapter_count,
                             (uint32_t)base_ms);
    }
    if (index < 0) {
        if (p->chapter_skip_cursor >= 0 && now < p->chapter_skip_deadline_us) {
            p->chapter_skip_deadline_us = now + 3000000ULL;
        }
        ppa_overlay_post(PPA_OVERLAY_KEY_CHAPTER, PPA_OVERLAY_CENTER, 1500,
                         PPA_OVERLAY_UTF8 | PPA_OVERLAY_WHITE,
                         direction < 0 ? ppa_playback_ui_text("chapter.first", "Already at first chapter") :
                                         ppa_playback_ui_text("chapter.last", "Already at last chapter"));
        return 1;
    }
    ppu_seek_request_submit((struct ppu_seek_request *)&p->seek_request,
        ppa_seek_controller_clamp_target_ms(file->chapters[index].time_ms,
            file->duration_ms), (uint32_t)(now / 1000ULL),
        PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER);
    p->paused = 0;
    ppa_session_set_paused(0);
    p->chapter_skip_cursor = index;
    p->chapter_skip_deadline_us = now + 3000000ULL;
    p->show_interface = 1;
    p->interface_hide_deadline_us = now + 3000000ULL;
    ppa_overlay_postf(PPA_OVERLAY_KEY_CHAPTER, PPA_OVERLAY_CENTER, 3000,
                      PPA_OVERLAY_UTF8 | PPA_OVERLAY_WHITE,
                      direction < 0 ? ppa_playback_ui_text("chapter.back", "Skip back to Chapter %d of %u\n%s") :
                                      ppa_playback_ui_text("chapter.forward", "Skip forward to Chapter %d of %u\n%s"),
                      index + 1, file->chapter_count,
                      file->chapters[index].title[0] != 0 ?
                          file->chapters[index].title : ppa_playback_ui_text("chapter.untitled", "Untitled chapter"));
    return 1;
}

static void mkv_input(volatile struct mkv_play_struct *p)
{
    SceCtrlData event, held;
    unsigned int key;
    int action;
    ppa_session_keep_awake();
    ctrl_read_sample_wait(&event, 16667U);
    ctrl_peek_sample(&held);
    key = event.Buttons;
    /* Session power policy observes held input; controls consume only sampler
     * events. A chord's release never returns as a transport command. */
    held.Buttons |= key;
    action = ppa_session_service(&held, p->paused);
    if ((action & PPA_SESSION_PAUSE) && !p->paused) {
        p->paused = 1;
        ppa_playback_control_pause();
    }
    ppa_playback_ui_tick_audio_only();
    if (action & PPA_SESSION_CONSUME_INPUT) {
        ctrl_flush();
        return;
    }
    if (ppa_session_audio_only()) {
        switch (key) {
        case PSP_CTRL_TRIANGLE:
            p->return_result = "exit: manual"; p->return_request = 1; break;
        case PSP_CTRL_SQUARE:
            p->paused = !p->paused;
            if (p->paused) ppa_playback_control_pause();
            else ppa_playback_control_resume();
            break;
        case PSP_CTRL_SELECT:
            p->audio_stream = (p->audio_stream + 1) % p->decoder.reader.file.audio_tracks;
            ppa_playback_control_audio_track_change(p->audio_stream);
            break;
        case PSP_CTRL_RTRIGGER:
            if (p->volume_boost < 6) ++p->volume_boost;
            pcm_set_normalize_ratio(p->volume_boost); break;
        case PSP_CTRL_LTRIGGER:
            if (p->volume_boost > 0) --p->volume_boost;
            pcm_set_normalize_ratio(p->volume_boost); break;
        default: break;
        }
        return;
    }
    if (p->interface_hide_deadline_us &&
        cpu_clock_auto_now_us() >= p->interface_hide_deadline_us) {
        p->show_interface = 0;
        p->interface_hide_deadline_us = 0;
    }
    /* These physical X-modifier actions are identical in both control types.
     * They work on chord-down in either order, also while paused. */
    switch (key) {
    case PSP_CTRL_CROSS | PSP_CTRL_UP:
        if (p->zoom < 200) { p->zoom += 5; ppa_playback_control_zoom_change(p->zoom); }
        return;
    case PSP_CTRL_CROSS | PSP_CTRL_DOWN:
        if (p->zoom > 100) { p->zoom -= 5; ppa_playback_control_zoom_change(p->zoom); }
        return;
    case PSP_CTRL_CROSS | PSP_CTRL_RTRIGGER:
        p->loop = !p->loop; return;
    case PSP_CTRL_CROSS | PSP_CTRL_LTRIGGER:
        mkv_play_toggle_subtitle_track_from_input(p);
        return;
    case PSP_CTRL_CROSS | PSP_CTRL_SQUARE:
        p->subtitle_fontcolor = (p->subtitle_fontcolor + 1) % NUMBER_OF_FONTCOLORS;
        gu_font_color_set(FONTCOLORS[p->subtitle_fontcolor]);
        ppa_playback_control_subtitle_style_change((int)p->subtitle_fontcolor);
        return;
    case PSP_CTRL_CROSS | PSP_CTRL_CIRCLE:
        p->subtitle_bordercolor = (p->subtitle_bordercolor + 1) % NUMBER_OF_BORDERCOLORS;
        gu_font_border_color_set(BORDERCOLORS[p->subtitle_bordercolor]);
        ppa_playback_control_subtitle_style_change(-(int)p->subtitle_bordercolor - 1);
        return;
    case PSP_CTRL_CROSS | PSP_CTRL_SELECT:
        p->audio_channel = p->audio_channel > -1 ? p->audio_channel - 1 : 1;
        ppa_playback_control_audio_channel_change(p->audio_channel); return;
    case PSP_CTRL_CROSS | PSP_CTRL_START:
        gu_lcd_output_inversion_set();
        ppa_playback_control_output_mode_change(-1); return;
    default: break;
    }
    /* Left/right arrive on down and fixed 4 Hz held ticks; other singles
     * retain release semantics. Selection has no decoder/governor side
     * effects; the producer commits the newest pending target. */
    switch (key) {
    case PSP_CTRL_TRIANGLE:
        p->return_result = "exit: manual"; p->return_request = 1; break;
    case PSP_CTRL_SQUARE:
        p->paused = !p->paused;
        if (p->paused) ppa_playback_control_pause();
        else ppa_playback_control_resume();
        break;
    case PSP_CTRL_LEFT:
    case PSP_CTRL_RIGHT: {
        int direction = key == PSP_CTRL_RIGHT ? 1 : -1;
        uint32_t now_ms = (uint32_t)(cpu_clock_auto_now_us() / 1000ULL);
        if (!p->decoder.reader.file.info || p->decoder.reader.file.info->total_indexes <= 0) {
            ppa_overlay_post(PPA_OVERLAY_KEY_SEEK, PPA_OVERLAY_TOP_RIGHT,
                1500, PPA_OVERLAY_UTF8, ppa_playback_ui_text(
                    "seek.no_cues", "Seek unavailable: MKV has no cue index"));
            break;
        }
        int base = ppu_seek_request_cursor((struct ppu_seek_request *)&p->seek_request,
            p->current_timestamp, p->seek_in_flight != 0U, now_ms);
        int target = ppa_seek_controller_target_ms(base, direction,
            p->decoder.reader.file.duration_ms);
        if (target != base)
            ppu_seek_request_submit((struct ppu_seek_request *)&p->seek_request,
                target, now_ms, 0U);
        p->chapter_skip_cursor = -1;
        p->chapter_skip_deadline_us = 0;
        p->paused = 0;
        ppa_session_set_paused(0);
        mkv_post_seek_label(p, direction, target);
        break;
    }
    case PSP_CTRL_LTRIGGER: mkv_chapter_skip(p, -1); break;
    case PSP_CTRL_RTRIGGER: mkv_chapter_skip(p, 1); break;
    case PSP_CTRL_CIRCLE:
        if (p->paused) make_screenshot();
        else { p->interface_hide_deadline_us = 0; p->show_interface = !p->show_interface; }
        break;
    case PSP_CTRL_SELECT:
        p->audio_stream = (p->audio_stream + 1) % p->decoder.reader.file.audio_tracks;
        ppa_playback_control_audio_track_change(p->audio_stream); break;
    case PSP_CTRL_UP:
        if (p->volume_boost < 6) ++p->volume_boost;
        pcm_set_normalize_ratio(p->volume_boost); break;
    case PSP_CTRL_DOWN:
        if (p->volume_boost > 0) --p->volume_boost;
        pcm_set_normalize_ratio(p->volume_boost); break;
    case PSP_CTRL_START:
        p->aspect_ratio = (p->aspect_ratio + 1) % number_of_aspect_ratios;
        ppa_playback_control_aspect_change(p->aspect_ratio); break;
    default: break;
    }
}

static int mkv_output_thread(SceSize input_length, void *input) {
	volatile struct mkv_play_struct *p = *((void **) input);

	p->current_audio_buffer_number = 0;
	int wait;
	int first = 1;
	SceInt32 volume = 0;
	
	while (p->return_request == 0) {
		if (!ppa_session_pause_point(&p->return_request, &p->paused, PPA_SESSION_WORKER_AUDIO)) break;
		volatile struct mkv_decode_buffer_struct *current_buffer = &p->decoder.output_audio_frame_buffers[p->current_audio_buffer_number];
		
		wait = mkv_wait(p,
                p->semaphore_can_get_audio,
                "mkv_output_thread: sceKernelWaitSema failed on semaphore_can_get_audio");
		if ( wait == -1) {
			break;
		}
		else if ( wait == 1 ) {
			/* Keep target audio alive until the target video reaches scanout.
			 * Earlier preroll buffers are discarded, but consuming target audio
			 * here would let the muted cursor run ahead and trigger catch-up drops.
			 * The producer stops queuing audio at this boundary and keeps decoding
			 * video, so holding this real buffer cannot fill the audio output ring. */
			while (p->return_request == 0 && p->seek_in_flight &&
			       p->seek_in_flight_epoch == p->decoder.output_epoch &&
			       current_buffer->epoch == p->decoder.output_epoch &&
			       current_buffer->timestamp >= p->seek_in_flight_target_ms) {
				if (ppa_process_exit_requested()) { p->return_request = 1; break; }
				sceKernelDelayThread(1000U);
			}
			if (p->return_request) break;
			if (current_buffer->epoch != p->decoder.output_epoch) {
                ppa_session_audio_only_consumed();
				unsigned int stale_epoch = current_buffer->epoch;
				(void)stale_epoch;
				p->current_audio_buffer_number =
					(p->current_audio_buffer_number + 1) %
					p->decoder.number_of_frame_buffers;
				if (sceKernelSignalSema(p->semaphore_can_put_audio, 1) < 0) {
					p->return_result =
						"mkv_output_thread: stale audio release failed";
					p->return_request = 1;
					break;
				}
				continue;
			}
			if ( volume < PSP_AUDIO_VOLUME_MAX ) {
				volume += PSP_AUDIO_VOLUME_MAX/5;
				if ( volume >= PSP_AUDIO_VOLUME_MAX ) 
					volume = PSP_AUDIO_VOLUME_MAX;
			}
			if (p->seek_in_flight == 0U &&
			    p->trickplay_settle_pending == 0U &&
			    !ppa_seek_controller_active(
			        (const struct ppa_seek_controller *)&p->seek_controller)) {
                int output_result = ppa_session_audio_output_epoch(
                    current_buffer->epoch, current_buffer->timestamp,
                    p->decoder.reader.file.audio_resample_scale,
                    p->decoder.reader.file.audio_rate, volume,
                    current_buffer->data);
                if (output_result < 0) {
                    p->return_result = "playback: audio output failed";
                    p->return_request = 1;
                    break;
                }
                if (output_result > 0 && !p->seek_in_flight &&
                    current_buffer->epoch == p->decoder.output_epoch)
                    p->current_timestamp = current_buffer->timestamp;
			}
			ppa_session_audio_only_consumed();
			if (ppa_session_audio_only()) {
				cpu_clock_auto_on_audio_only_complete(p->decoder.audio_frame_duration);
			}
			p->current_audio_buffer_number = (p->current_audio_buffer_number + 1) % p->decoder.number_of_frame_buffers;
			if (first == 1) {
				first = 0;
			}
			else {	
				if (sceKernelSignalSema(p->semaphore_can_put_audio, 1) < 0) {
					p->return_result  = "mkv_output_thread: sceKernelSignalSema failed on semaphore_can_put_audio";
					p->return_request = 1;
					break;
				}
			}
			
		}
		else {
			sceKernelDelayThread(PPA_SEMA_IDLE_DELAY_US);
		}

		ppa_session_pause_point(&p->return_request, &p->paused, PPA_SESSION_WORKER_AUDIO);
	}

	return(0);
}

void mkv_play_reset(volatile struct mkv_play_struct *p) {
    ppu_seek_request_reset((struct ppu_seek_request *)&p->seek_request);

	if (ppa_seek_controller_active(
	        (const struct ppa_seek_controller *)&p->seek_controller))
		mkv_play_trickplay_end(p);
	else
		ppa_video_pipeline_set_trickplay_frame_drop(0);
	ppa_playback_control_reset();
	p->trickplay_settle_pending = 0U;
	p->seek_audio_target_queued = 0U;
	p->seek_in_flight = 0U;
	p->seek_in_flight_epoch = 0U;
	p->seek_in_flight_target_ms = 0;
	ppa_seek_controller_reset((struct ppa_seek_controller *)&p->seek_controller);
    if (mkv_play_begin_seek(p, 0)) {
        char *result = mkv_decode_seek((struct mkv_decode_struct *)&p->decoder, 0, 0);
        if (result) { p->return_result = result; p->return_request = 1; }
        else { p->current_timestamp = 0; p->seek_transactions++; }
    }
}

static int mkv_seek_audio_can_decode(volatile struct mkv_play_struct *p)
{
	return !p->seek_in_flight ||
	       p->seek_in_flight_epoch != p->decoder.output_epoch ||
	       !p->seek_audio_target_queued;
}

static char *mkv_audio_only_decode(void *owner, unsigned int *codec_us)
{
    volatile struct mkv_play_struct *p = owner;
    pcm_set_normalize_ratio(p->volume_boost);
    return mkv_decode_get_audio((struct mkv_decode_struct *)&p->decoder,
        p->audio_stream, p->audio_channel, 1, p->volume_boost, codec_us);
}
static int mkv_audio_only_eof(void *owner, const char *error)
{
    (void)owner;
    return strcmp(error, PPU_AUDIO_EOF) == 0;
}
static void mkv_audio_only_progress(void *owner)
{ (void)owner; }

static void mkv_drain_audio_tail(volatile struct mkv_play_struct *p)
{
    struct ppa_audio_only_adapter a;
    volatile int complete = 0;
    memset(&a, 0, sizeof(a));
    a.owner = (void *)p; a.stop = &p->return_request; a.paused = &p->paused;
    a.eof = &complete; a.result = &p->return_result;
    a.can_put = p->semaphore_can_put_audio; a.can_get = p->semaphore_can_get_audio;
    a.decode = mkv_audio_only_decode; a.at_eof = mkv_audio_only_eof;
    a.progress = mkv_audio_only_progress; a.duration_ms = p->decoder.audio_frame_duration;
    a.drain_only = 1;
    ppa_session_run_audio_only(&a);
}

static int mkv_demux_thread(SceSize input_length, void *input) {
	volatile struct mkv_play_struct *p = *((void **) input);
	if (ppa_session_audio_only()) {
		struct ppa_audio_only_adapter a;
		memset(&a, 0, sizeof(a));
		a.owner = (void *)p;
		a.stop = &p->return_request; a.paused = &p->paused;
		a.eof = &p->decoder.is_eof; a.result = &p->return_result;
		a.can_put = p->semaphore_can_put_audio; a.can_get = p->semaphore_can_get_audio;
		a.decode = mkv_audio_only_decode; a.at_eof = mkv_audio_only_eof;
		a.progress = mkv_audio_only_progress; a.duration_ms = p->decoder.audio_frame_duration;
		return ppa_session_run_audio_only(&a);
	}


	(void)input_length;

	int cached_video_frame = 0;
	uint64_t pipeline_last_progress_us = cpu_clock_auto_now_us();
	unsigned int pipeline_watchdog_fallback_done = 0U;

	while (p->return_request == 0 &&
		!mkv_decode_is_eof((struct mkv_decode_struct *) &p->decoder)) {
		if (!ppa_session_pause_point(&p->return_request, &p->paused, PPA_SESSION_WORKER_DECODE)) break;
		int wait;
		char *result;

		/* Output threads intentionally sleep while paused. The 6.3.41B demux
		 * thread did not, so it repeatedly reserved/returned a video slot at
		 * the audio high-water mark and produced thousands of backpressure
		 * events without consuming input. Park the producer as well. */
		if (p->paused == 1) {
			pipeline_last_progress_us = cpu_clock_auto_now_us();
			pipeline_watchdog_fallback_done = 0U;
			p->h264x_fixed_cadence_valid = 0U;
			mkv_h264x_vblank_cadence_reset();
			sceKernelDelayThread(100000);
			continue;
		}

		mkv_play_service_subtitle_font_reload(p);

		wait = mkv_seek_audio_can_decode(p) ? mkv_wait(p,
		                p->semaphore_can_put_audio,
		                "mkv_play_start: sceKernelWaitSema failed on semaphore_can_put_audio") : 0;

		if (wait == -1) {
			break;
		}
		else if (wait == 1) {
			{
				struct ppa_bus_token decode_bus = ppa_bus_begin(PPA_BUS_DECODE);
				result = mkv_decode_get_audio((struct mkv_decode_struct *) &p->decoder,
			                              p->audio_stream,
			                              p->audio_channel,
			                              1,
			                              p->volume_boost, 0);
				ppa_bus_end(&decode_bus);
			}

			if (result != 0) {
				if (strcmp(result, MKV_READ_AUDIO_VIDEO_BACKPRESSURE) == 0) {
					if (sceKernelSignalSema(p->semaphore_can_put_audio, 1) < 0) {
						p->return_result =
							"mkv_play_start: sceKernelSignalSema failed on audio backpressure";
						p->return_request = 1;
						break;
					}
				}
				else {
					p->return_result  = result;
					p->return_request = 1;
					break;
				}
			}
			else {
				if (p->seek_in_flight &&
				    p->seek_in_flight_epoch == p->decoder.output_epoch &&
				    p->decoder.last_audio_timestamp >= p->seek_in_flight_target_ms)
					p->seek_audio_target_queued = 1U;
				if (ppa_session_audio_publish(p->semaphore_can_get_audio) < 0) {
					p->return_result  = "mkv_play_start: sceKernelSignalSema failed on semaphore_can_get_audio";
					p->return_request = 1;
					break;
				}

				pipeline_last_progress_us = cpu_clock_auto_now_us();
				pipeline_watchdog_fallback_done = 0U;
			}
		}

		wait = mkv_wait(p,
		                p->semaphore_can_put_video,
		                "mkv_play_start: sceKernelWaitSema failed on semaphore_can_put_video");

		if (wait == -1) {
			break;
		}
		else if (wait == 1) {
			if (cached_video_frame > 0) {
				mkv_play_service_subtitle_font_reload(p);

				{
					struct ppa_bus_token decode_bus = ppa_bus_begin(PPA_BUS_DECODE);
					result = mkv_decode_get_cached_video((struct mkv_decode_struct *) &p->decoder,
													cached_video_frame,
													p->audio_stream,
													p->volume_boost,
													p->aspect_ratio,
													p->zoom,
													ppa_video_pipeline_extreme_battery_saver_enabled() ?
												0U : p->luminosity_boost,
													p->show_interface,
													p->subtitle,
													p->subtitle_format,
													p->loop);
					ppa_bus_end(&decode_bus);
				}

				if (result != 0) {
					p->return_result  = result;
					p->return_request = 1;
					break;
				}

				cached_video_frame--;

				if (sceKernelSignalSema(p->semaphore_can_get_video, 1) < 0) {
					p->return_result  = "mkv_play_start: sceKernelSignalSema failed on semaphore_can_get_video";
					p->return_request = 1;
					break;
				}

				pipeline_last_progress_us = cpu_clock_auto_now_us();
				pipeline_watchdog_fallback_done = 0U;
			}
			else {
				mkv_play_service_subtitle_font_reload(p);

				{
					struct ppa_bus_token decode_bus = ppa_bus_begin(PPA_BUS_DECODE);
					result = mkv_decode_get_video((struct mkv_decode_struct *) &p->decoder,
											p->audio_stream,
											p->volume_boost,
											p->aspect_ratio,
											p->zoom,
											ppa_video_pipeline_extreme_battery_saver_enabled() ?
												0U : p->luminosity_boost,
											p->show_interface,
											p->subtitle,
											p->subtitle_format,
											p->loop,
											&cached_video_frame);
					ppa_bus_end(&decode_bus);
				}

				if (result != 0) {
					if (strcmp(result, MKV_READ_VIDEO_AUDIO_BACKPRESSURE) == 0) {
						/* No input block and no decoder state were consumed. Return the
						 * reserved output slot, let the audio passes below drain the
						 * compressed queue, and retry video on a later iteration. */

						if (sceKernelSignalSema(p->semaphore_can_put_video, 1) < 0) {
							p->return_result =
								"mkv_play_start: sceKernelSignalSema failed on video backpressure";
							p->return_request = 1;
							break;
						}
					}
					else {
						p->return_result  = result;
						p->return_request = 1;
						break;
					}
				}
				else if (cached_video_frame >= 0) {
					if (sceKernelSignalSema(p->semaphore_can_get_video, 1) < 0) {
						p->return_result  = "mkv_play_start: sceKernelSignalSema failed on semaphore_can_get_video";
						p->return_request = 1;
						break;
					}

				pipeline_last_progress_us = cpu_clock_auto_now_us();
				pipeline_watchdog_fallback_done = 0U;
				}
				else {
					if (sceKernelSignalSema(p->semaphore_can_put_video, 1) < 0) {
						p->return_result  = "mkv_play_start: sceKernelSignalSema failed on semaphore_can_put_video1";
						p->return_request = 1;
						break;
					}
				}
			}
		}

		if (!p->decoder.is_eof && p->decoder.reader.file.audio_up_sample == 0 &&
		    mkv_seek_audio_can_decode(p)) {
			wait = mkv_wait(p,
			                p->semaphore_can_put_audio,
			                "mkv_play_start: sceKernelWaitSema failed on semaphore_can_put_audio");

			if (wait == -1) {
				break;
			}
			else if (wait == 1) {
				{
					struct ppa_bus_token decode_bus = ppa_bus_begin(PPA_BUS_DECODE);
					result = mkv_decode_get_audio((struct mkv_decode_struct *) &p->decoder,
				                              p->audio_stream,
				                              p->audio_channel,
				                              1,
				                              p->volume_boost, 0);
					ppa_bus_end(&decode_bus);
				}

				if (result != 0) {
					if (strcmp(result, MKV_READ_AUDIO_VIDEO_BACKPRESSURE) == 0) {
						if (sceKernelSignalSema(p->semaphore_can_put_audio, 1) < 0) {
							p->return_result =
								"mkv_play_start: sceKernelSignalSema failed on audio backpressure";
							p->return_request = 1;
							break;
						}
					}
					else {
						p->return_result  = result;
						p->return_request = 1;
						break;
					}
				}
				else {
					if (p->seek_in_flight &&
					    p->seek_in_flight_epoch == p->decoder.output_epoch &&
					    p->decoder.last_audio_timestamp >= p->seek_in_flight_target_ms)
						p->seek_audio_target_queued = 1U;
					if (ppa_session_audio_publish(p->semaphore_can_get_audio) < 0) {
						p->return_result  = "mkv_play_start: sceKernelSignalSema failed on semaphore_can_get_audio";
						p->return_request = 1;
						break;
					}

					pipeline_last_progress_us = cpu_clock_auto_now_us();
					pipeline_watchdog_fallback_done = 0U;
				}
			}
		}

		if (p->seek == 0 && !p->seek_in_flight &&
		    ppa_seek_controller_active(
		        (const struct ppa_seek_controller *)&p->seek_controller)) {
			p->seek = ppa_seek_controller_take_due_step(
				(struct ppa_seek_controller *)&p->seek_controller,
				cpu_clock_auto_now_us());
		}

		if (mkv_play_do_seek(p)) {
			cached_video_frame = 0;
			pipeline_last_progress_us = cpu_clock_auto_now_us();
			pipeline_watchdog_fallback_done = 0U;
		}

		/* A malformed/deep stream can stop all four bounded queues without
		 * producing a fatal decoder return. First demote the expensive deep
		 * presentation policy to the proven baseline while preserving decoded
		 * frames. If no producer progress follows, terminate through the normal
		 * bounded shutdown path instead of leaving an unresponsive player. */
		if (p->paused == 0 && p->return_request == 0) {
			uint64_t now_us = cpu_clock_auto_now_us();
			uint64_t stalled_us = now_us >= pipeline_last_progress_us ?
				now_us - pipeline_last_progress_us : 0U;
			if (stalled_us >= 2000000ULL &&
			    pipeline_watchdog_fallback_done == 0U) {
				pipeline_watchdog_fallback_done = 1U;
				p->h264x_playback_mode = 0U;
				p->h264x_output_remap_policy =
					mkv_h264x_motion_mode_remap_policy(0U);
				p->h264x_restore_swap_pending = 0U;
				p->h264x_restore_swap_target_timestamp = -1;
				p->h264x_fixed_cadence_valid = 0U;
				mkv_h264x_vblank_cadence_reset();
				mkv_decode_h264x_dual_dpb_mode_set(0U);
				mkv_decode_h264x_output_remap_set(
					(struct mkv_decode_struct *)&p->decoder,
					p->h264x_output_remap_policy);
				ppa_overlay_post(PPA_OVERLAY_KEY_INFO,
				                 PPA_OVERLAY_TOP_RIGHT, 1800,
				                 PPA_OVERLAY_UTF8,
				                 "[i]Playback recovery: baseline presentation[/i]");
			}
			else if (stalled_us >= 6000000ULL &&
			         pipeline_watchdog_fallback_done != 0U) {
				p->return_result =
					"mkv playback watchdog: pipeline made no progress";
				p->return_request = 1;
				break;
			}
		}

		if (mkv_decode_is_eof((struct mkv_decode_struct *) &p->decoder)) {
            mkv_drain_audio_tail(p);
            if (p->return_request) break;
			if (p->loop == 1) {
				cached_video_frame = 0;
				mkv_play_reset(p);
			}
			else {
                p->audio_tail_done = 1;
				p->return_request = 1;
				break;
			}
		}

	}

	return(0);
}

char *mkv_play_start(volatile struct mkv_play_struct *p) {
    enum ctrl_context previous_context = ctrl_set_context(
        ppa_session_audio_only() ? CTRL_CONTEXT_AUDIO_ONLY : CTRL_CONTEXT_PLAYBACK);

	ppa_frame_sink_reset_timing();
	ppa_session_start();
	ppa_playback_ui_tick_audio_only();

    /* Persistent libsamplerate history belongs to the decode owner. The
     * legacy endpoint-based VFPU resampler cannot share that state. */
    ppa_audio_accel_session_begin(PPU_AUDIO_BLOCK, PPU_AUDIO_BLOCK, 0);

	if (ppa_session_start_workers(p->output_thread, p->show_thread, p->demux_thread,
	                              sizeof(p), &p, &p->return_request) < 0) {
		ppa_audio_accel_session_end();
		ctrl_set_context(previous_context);
		return "playback: worker start failed";
	}


	while (p->return_request == 0 &&
	       (!mkv_decode_is_eof((struct mkv_decode_struct *) &p->decoder) ||
            (!ppa_session_audio_only() && !p->audio_tail_done))) {
		if (ppa_process_exit_requested()) { p->return_request = 1; break; }
		mkv_input(p);
		/* Audio cadence belongs to the blocking output worker. A larger control
		 * yield complements the input event wait; normal A/V retains its
		 * existing low-latency controller/seek polling policy. */
		sceKernelDelayThread(ppa_session_audio_only() ? 10000U : 1000U);
	}

	if (mkv_decode_is_eof((struct mkv_decode_struct *) &p->decoder)) {
		p->last_keyframe_pos = 0;
	}

	/* Stop first, let bounded queue waits observe stop, then join. The old
	 * unconditional one-second sleep let audio/video workers continue drifting
	 * and an in-flight diagnostic decode could make Triangle appear ignored. */
	p->return_request = 1;
	/* Queue waits poll with an 8 ms bound. Let them observe stop without
	 * inventing producer credits or consumer frames during a pending swap. */

	{
		int audio_join, show_join, decode_join;
		audio_join = ppa_wait_thread_end_safe(p->output_thread, "MKV", "audio",
		    PPA_H264X_SHUTDOWN_WAIT_US, PPA_H264X_SHUTDOWN_FORCE_WAIT_US);
		show_join = ppa_wait_thread_end_safe(p->show_thread, "MKV", "video_show",
		    PPA_H264X_SHUTDOWN_WAIT_US, PPA_H264X_SHUTDOWN_FORCE_WAIT_US);
		decode_join = ppa_wait_thread_end_safe(p->demux_thread, "MKV", "demux_decode",
		    PPA_H264X_SHUTDOWN_WAIT_US, PPA_H264X_SHUTDOWN_FORCE_WAIT_US);
		if (audio_join < 0 || show_join < 0 || decode_join < 0)
			ppa_wait_quarantine("MKV worker did not return",
			    decode_join < 0 ? decode_join : (show_join < 0 ? show_join : audio_join));
	}
	ppa_audio_accel_session_end();

	/* A stream may end after rotation start but before the cached partner is
	 * consumed. Account for that explicitly and release the application-side
	 * pair so close always reports a balanced, non-pending state. */
	if (p->h264x_restore_swap_pending) {
		unsigned int pending_policy;
		pending_policy = p->h264x_restore_swap_policy;
		
		p->h264x_restore_swap_pending = 0U;
		p->h264x_restore_swap_policy = 0U;
		p->h264x_restore_swap_pair_id = 0U;
		p->h264x_restore_swap_target_native_seq = 0U;
		p->h264x_restore_swap_target_poc_lsb = 0U;
		p->h264x_restore_swap_target_timestamp = -1;
	}

	ctrl_set_context(previous_context);
	return(p->return_result);
}

static void mkv_play_subtitle_label(struct mkv_play_struct *p,
                                    int subtitle_index,
                                    char *out,
                                    int out_size)
{
	const char *filename;
	const char *prefix = "mkv subtitle track(";
	const char *lang;
	const char *lang_name;
	long parsed_tracknum;
	int32_t tracknum;
	int track_index;
	mkvinfo_track_t *track;

	if (out == 0 || out_size <= 0)
		return;

	out[0] = '\0';

	if (p == 0 ||
	    subtitle_index < 0 ||
	    subtitle_index >= (int)p->subtitle_count) {
		snprintf(out, out_size, "Subtitle");
		return;
	}

	filename = subtitle_parser[subtitle_index].filename;

	if (strncmp(filename, prefix, strlen(prefix)) != 0) {
		snprintf(out, out_size, "%s", filename);
		return;
	}

	parsed_tracknum = -1;
	if (sscanf(filename + strlen(prefix), "%ld", &parsed_tracknum) != 1) {
		snprintf(out, out_size, "%s", filename);
		return;
	}

	tracknum = (int32_t)parsed_tracknum;

	track_index = mkv_play_find_track_by_tracknum(p, tracknum);
	if (track_index < 0) {
		snprintf(out, out_size, "Subtitle track %ld", (long)tracknum);
		return;
	}

	track = p->decoder.reader.file.info->tracks[track_index];
	if (track == 0) {
		snprintf(out, out_size, "Subtitle track %ld", (long)tracknum);
		return;
	}

	lang = track->language_ietf[0] ? track->language_ietf : track->language;
	lang_name = mkv_play_language_display_name(lang);

	if (track->name[0] != '\0') {
		snprintf(out,
		         out_size,
		         "%s [%s] - %s%s",
		         lang_name,
		         lang && lang[0] ? lang : "und",
		         track->name,
		         track->flag_forced ? " (forced)" : "");
	}
	else {
		snprintf(out,
		         out_size,
		         "%s [%s]%s",
		         lang_name,
		         lang && lang[0] ? lang : "und",
		         track->flag_forced ? " (forced)" : "");
	}
}

static void mkv_battery_sort_timestamps(int *values, unsigned int count)
{
	unsigned int i;
	for (i = 1U; i < count; ++i) {
		int value = values[i];
		unsigned int j = i;
		while (j > 0U && values[j - 1U] > value) {
			values[j] = values[j - 1U];
			j--;
		}
		values[j] = value;
	}
}

static int mkv_battery_stream_quick_probe(struct mkv_decode_struct *decoder)
{
	int timestamps[24];
	unsigned int audio_sizes[24];
	unsigned int video_count = 0U;
	unsigned int picture_count = 0U;
	unsigned int audio_count = 0U;
	unsigned int b_run = 0U;
	unsigned int attempts;
	int compatible = 1;
	int has_audio;
	unsigned int required_audio_samples;

	if (decoder == 0)
		return 0;
	has_audio = decoder->reader.file.audio_tracks > 0;
	required_audio_samples =
		decoder->reader.file.duration_ms < 2000U ? 1U : 4U;
	for (attempts = 0U; attempts < 96U && compatible; ++attempts) {
		int made_progress = 0;
		if (video_count < 24U) {
			struct mkv_read_output_struct packet;
			char *result;
			memset(&packet, 0, sizeof(packet));
			result = mkv_read_get_video(&decoder->reader, &packet);
			if (result == 0) {
				h264x_probe_before_decode(&decoder->h264x, packet.data,
				                          packet.size, packet.timestamp,
				                          picture_count == 0U ? 3 : 0);
				if (decoder->h264x.current_au.parse_error ||
				    decoder->h264x.current_au.is_reference_b)
					compatible = 0;
				if (decoder->h264x.current_au.is_b) {
					b_run++;
					if (b_run > 1U)
						compatible = 0;
				}
				else if (decoder->h264x.current_au.is_i ||
				         decoder->h264x.current_au.is_p) {
					b_run = 0U;
				}
				if (decoder->h264x.current_au.is_i ||
				    decoder->h264x.current_au.is_p ||
				    decoder->h264x.current_au.is_b)
					picture_count++;
				timestamps[video_count++] = packet.timestamp;
				mkv_read_release_output(&packet);
				made_progress = 1;
			}
			else if (strcmp(result, MKV_READ_VIDEO_AUDIO_BACKPRESSURE) != 0) {
				/* EOF is acceptable after a representative short-file sample. */
				if (video_count == 0U)
					compatible = 0;
			}
		}
		if (has_audio && audio_count < 24U) {
			struct mkv_read_output_struct packet;
			char *result;
			memset(&packet, 0, sizeof(packet));
			result = mkv_read_get_audio(&decoder->reader, 0U, &packet);
			if (result == 0) {
				audio_sizes[audio_count++] = packet.size;
				mkv_read_release_output(&packet);
				made_progress = 1;
			}
			else if (strcmp(result, MKV_READ_AUDIO_VIDEO_BACKPRESSURE) != 0 &&
			         audio_count < required_audio_samples && video_count >= 24U) {
				compatible = 0;
			}
		}
		if (video_count >= 24U && (!has_audio || audio_count >= 24U))
			break;
		if (!made_progress && video_count >= 1U &&
		    (!has_audio || audio_count >= required_audio_samples))
			break;
	}

	if (compatible && video_count >= 2U) {
		unsigned int i;
		int nominal_delta;
		mkv_battery_sort_timestamps(timestamps, video_count);
		nominal_delta = timestamps[1] - timestamps[0];
		if (nominal_delta <= 0)
			compatible = 0;
		for (i = 2U; compatible && i < video_count; ++i) {
			int delta = timestamps[i] - timestamps[i - 1U];
			int difference = delta - nominal_delta;
			if (difference < 0)
				difference = -difference;
			/* Matroska timecodes are integer ticks; one millisecond of
			 * quantization tolerance still rejects variable cadence. */
			if (delta <= 0 || difference > 1)
				compatible = 0;
		}
	}
	else if (video_count == 0U) {
		compatible = 0;
	}

	if (compatible && has_audio) {
		uint64_t total = 0ULL;
		unsigned int minimum = 0xffffffffU;
		unsigned int maximum = 0U;
		unsigned int average;
		unsigned int i;
		if (audio_count < required_audio_samples)
			compatible = 0;
		for (i = 0U; compatible && i < audio_count; ++i) {
			if (audio_sizes[i] < minimum) minimum = audio_sizes[i];
			if (audio_sizes[i] > maximum) maximum = audio_sizes[i];
			total += audio_sizes[i];
		}
		average = audio_count == 0U ? 0U : (unsigned int)(total / audio_count);
		if (average == 0U ||
		    (uint64_t)(maximum - minimum) * 100ULL >
		        (uint64_t)average * 40ULL)
			compatible = 0;
	}

	if (mkv_read_seek(&decoder->reader, 0, 0) != 0)
		compatible = 0;
	h264x_probe_reset_stream(&decoder->h264x);
	return compatible && picture_count != 0U;
}

static int mkv_extreme_battery_saver_compatible(
	struct mkv_decode_struct *decoder)
{
	const struct mkv_file_struct *file;
	const struct h264x_probe *probe;
	mkvinfo_track_t *video_track;
	unsigned int profile;

	if (decoder == 0)
		return 0;
	file = &decoder->reader.file;
	probe = &decoder->h264x;
	if (file->info == 0 || file->video_track_id < 0 ||
	    file->video_track_id >= file->info->total_tracks)
		return 0;
	video_track = file->info->tracks[file->video_track_id];
	if (video_track == 0 || video_track->duration == 0U)
		return 0;
	if (file->video_width == 0U || file->video_height == 0U ||
	    file->video_width > PPA_VIDEO_MAX_CODED_WIDTH ||
	    file->video_height > PPA_VIDEO_MAX_CODED_HEIGHT)
		return 0;
	if (!probe->sps.valid || !probe->pps.valid || !probe->avcc_valid)
		return 0;
	profile = probe->sps.profile_idc != 0U ?
		probe->sps.profile_idc : probe->avcc_profile_idc;
	if (profile != 66U && profile != 77U)
		return 0;
	if (probe->sps.level_idc > 30U ||
	    probe->sps.chroma_format_idc > 1U ||
	    probe->sps.bit_depth_luma_minus8 != 0U ||
	    probe->sps.bit_depth_chroma_minus8 != 0U ||
	    probe->sps.frame_mbs_only_flag == 0U ||
	    probe->sps.mb_adaptive_frame_field_flag != 0U ||
	    probe->sps.num_ref_frames >
	        (probe->pps.entropy_coding_mode_flag ? 2U : 3U) ||
	    probe->pps.num_slice_groups_minus1 != 0U ||
	    probe->pps.weighted_pred_flag != 0U ||
	    probe->pps.weighted_bipred_idc != 0U ||
	    probe->pps.transform_8x8_mode_flag != 0U ||
	    probe->pps.num_ref_idx_l1_default_active_minus1 != 0U)
		return 0;
	if (file->audio_tracks > 0) {
		int audio_id = file->audio_track_ids[0];
		mkvinfo_track_t *audio_track;
		if (audio_id < 0 || audio_id >= file->info->total_tracks)
			return 0;
		audio_track = file->info->tracks[audio_id];
		if (audio_track == 0 || audio_track->duration == 0U)
			return 0;
	}
	return mkv_battery_stream_quick_probe(decoder);
}

char *mkv_play_open(struct mkv_play_struct *p,
                    struct movie_file_struct *movie,
                    int usePos,
                    int pspType,
                    int tvAspectRatio,
                    int tvWidth,
                    int tvHeight,
                    int videoMode) {

	mkv_play_safe_constructor(p);
	p->subtitle = 0;
	p->subtitle_count = 0;

	if (movie == 0 || movie->movie_file[0] == '\0')
		return "mkv_play_open: invalid movie path";

	memset(p->movie_file, 0, sizeof(p->movie_file));
	{
		size_t path_size = strlen(movie->movie_file);
		if (path_size >= sizeof(p->movie_file))
			path_size = sizeof(p->movie_file) - 1U;
		memcpy(p->movie_file, movie->movie_file, path_size);
		p->movie_file[path_size] = 0;
	}

	char *result = mkv_decode_open(&p->decoder,
	                               movie->movie_file,
	                               pspType,
	                               tvAspectRatio,
	                               tvWidth,
	                               tvHeight,
	                               videoMode);

	if (result != 0) {
		mkv_play_close(p, 0, pspType);
		return(result);
	}
	if (!ppa_session_audio_only() && ppa_video_pipeline_extreme_battery_saver_enabled() &&
	    !mkv_extreme_battery_saver_compatible(&p->decoder)) {
		mkv_play_close(p, 0, pspType);
		return PPA_BATTERY_SAVER_REJECTED;
	}

	if (ppa_video_pipeline_extreme_battery_saver_enabled()) {
		p->h264x_playback_mode = 0U;
		p->h264x_output_remap_policy = 0U;
		p->h264x_restore_policy = 0U;
		p->decoder.h264x.enabled = 0;
		
		mkv_decode_h264x_dual_dpb_mode_set(0U);
	}
	else {
		mkv_decode_h264x_dual_dpb_mode_set(
			mkv_h264x_motion_mode_dual_dpb(p->h264x_playback_mode));
	}
	mkv_decode_h264x_output_remap_set(&p->decoder,
	                                  p->h264x_output_remap_policy);

	/* 6.3.35 keeps the validated immutable double-stage/NEXTFRAME path and
	 * adds three private CPU surfaces: previous trusted display, saved native
	 * candidate for triplet rotation, and generated interpolation output. */
	p->h264x_present_anchor_history = 0;
	p->h264x_present_anchor_history_bytes = 0U;
	p->h264x_present_anchor_history_valid = 0U;
	p->h264x_present_anchor_history_timestamp = -1;
	p->h264x_restore_saved_surface = 0;
	p->h264x_restore_generated_surface = 0;
	p->h264x_restore_surface_bytes = 0U;
	p->h264x_restore_swap_pending = 0U;
	p->h264x_restore_swap_policy = 0U;
	p->h264x_restore_swap_pair_id = 0U;
	p->h264x_restore_swap_target_native_seq = 0U;
	p->h264x_restore_swap_target_poc_lsb = 0U;
	p->h264x_restore_swap_target_timestamp = -1;
	p->h264x_stage_surface[0] = 0;
	p->h264x_stage_surface[1] = 0;
	g_h264x_deep_stage_surface = 0;
	g_h264x_last_submitted_surface = 0;
	g_h264x_deep_stage_index = 0U;
	p->h264x_stage_surface_bytes = 0U;
	{
		unsigned int stage_bytes;
		stage_bytes = (ppa_session_audio_only() || ppa_video_pipeline_extreme_battery_saver_enabled()) ?
			0U : mkv_h264x_present_surface_bytes(p);
		if (stage_bytes != 0U) {
			/* Native presentation already holds its decoder ring lease until a
			 * completed swap. Allocate extra immutable scanout stages only for
			 * the compatibility path that actually uses them. If future code
			 * enables compatibility in-band, it must provision these before use. */
			if (p->decoder.h264x_compat_active) {
				p->h264x_stage_surface[0] = malloc_64(stage_bytes);
				p->h264x_stage_surface[1] = malloc_64(stage_bytes);
				g_h264x_deep_stage_surface = malloc_64(stage_bytes);
			}
			p->h264x_present_anchor_history = malloc_64(stage_bytes);
			p->h264x_restore_saved_surface = malloc_64(stage_bytes);
			p->h264x_restore_generated_surface = malloc_64(stage_bytes);
			if (p->h264x_stage_surface[0] != 0 &&
			    p->h264x_stage_surface[1] != 0)
				p->h264x_stage_surface_bytes = stage_bytes;
			if (p->h264x_present_anchor_history != 0)
				p->h264x_present_anchor_history_bytes = stage_bytes;
			if (p->h264x_restore_saved_surface != 0 &&
			    p->h264x_restore_generated_surface != 0)
				p->h264x_restore_surface_bytes = stage_bytes;
			/* Keep startup out of the CPU raster path too. These are optional
			 * H264X restoration surfaces, so a failed GE clear simply leaves
			 * them invalid until their first complete copy or generated frame. */
			{
				unsigned int stage_pitch =
					(unsigned int)p->decoder.output_texture_width;
				unsigned int stage_height = stage_pitch != 0U ?
					stage_bytes / (stage_pitch * 4U) : 0U;
				void *stage_clear[6];
				unsigned int clear_index;

				stage_clear[0] = p->h264x_stage_surface[0];
				stage_clear[1] = p->h264x_stage_surface[1];
				stage_clear[2] = g_h264x_deep_stage_surface;
				stage_clear[3] = p->h264x_present_anchor_history;
				stage_clear[4] = p->h264x_restore_saved_surface;
				stage_clear[5] = p->h264x_restore_generated_surface;
				for (clear_index = 0U; clear_index < 6U; clear_index++) {
					if (stage_clear[clear_index] != 0 &&
					    ppa_gu_clear_frame_blocking(stage_clear[clear_index],
					                                stage_pitch, stage_pitch,
					                                stage_height) < 0)
						;
				}
			}
		}
	}

	if (!ppa_session_audio_only()) mkv_play_try_load_embedded_subtitle_font(p);

	if (!ppa_session_audio_only() && p->decoder.reader.file.subtitle_tracks > 0) {
		int subtitle_track = 0;

		while (p->subtitle_count < MAX_SUBTITLES &&
		       subtitle_track < p->decoder.reader.file.subtitle_tracks) {
			struct subtitle_parse_struct *cur_parser =
				&subtitle_parser[p->subtitle_count];

			subtitle_parse_safe_constructor(cur_parser);

			int32_t tracknum =
				p->decoder.reader.file.info
					->tracks[p->decoder.reader.file.subtitle_track_ids[subtitle_track]]
					->tracknum;

			snprintf(cur_parser->filename, sizeof(cur_parser->filename),
			        "mkv subtitle track(%ld)",
			        (long)tracknum);

			subtitle_track++;

			cur_parser->p_sub_frame =
				(struct subtitle_frame_struct *)malloc_64(sizeof(struct subtitle_frame_struct));

			if (cur_parser->p_sub_frame == 0) {
				subtitle_parse_close(cur_parser);
				continue;
			}

			subtitle_frame_safe_constructor(cur_parser->p_sub_frame);
			cur_parser->p_cur_sub_frame = cur_parser->p_sub_frame;

			p->subtitle_count++;
		}
	}

	if (!ppa_session_audio_only() && p->subtitle_count < MAX_SUBTITLES) {
		subtitle_parse_search(movie,
		                      p->decoder.reader.file.video_rate,
		                      p->decoder.reader.file.video_scale,
		                      &p->subtitle_count);
	}

	if (p->subtitle_count > 0) {
		p->subtitle = mkv_play_choose_preferred_subtitle(p);
	}
	else {
		p->subtitle = 0;
	}

	if (ppa_privileged_audio_set_frequency(
	                              p->decoder.reader.file.audio_rate) != 0) {
		mkv_play_close(p, 0, pspType);
		return("mkv_play_open: sceAudioSetFrequency failed");
	}

	p->audio_reserved =
		sceAudioChReserve(0,
		                  p->decoder.reader.file.audio_resample_scale,
		                  PSP_AUDIO_FORMAT_STEREO);

	if (p->audio_reserved < 0) {
		mkv_play_close(p, 0, pspType);
		return("mkv_play_open: sceAudioChReserve failed");
	}

	if (!ppa_session_audio_only()) {
	p->semaphore_can_get_video =
		sceKernelCreateSema("can_get_video",
		                    0,
		                    0,
		                    p->decoder.number_of_frame_buffers,
		                    0);

	if (p->semaphore_can_get_video < 0) {
		mkv_play_close(p, 0, pspType);
		return("mkv_play_open: sceKernelCreateSema failed on semaphore_can_get_video");
	}

	p->semaphore_can_put_video =
		sceKernelCreateSema("can_put_video",
		                    0,
		                    p->decoder.number_of_frame_buffers,
		                    p->decoder.number_of_frame_buffers,
		                    0);

	if (p->semaphore_can_put_video < 0) {
		mkv_play_close(p, 0, pspType);
		return("mkv_play_open: sceKernelCreateSema failed on semaphore_can_put_video");
	}

	}

    if (!ppu_seek_request_init(&p->seek_request)) {
        mkv_play_close(p, 0, pspType);
        return "playback: could not create seek request lock";
    }

	p->semaphore_can_get_audio =
		sceKernelCreateSema("can_get_audio",
		                    0,
		                    0,
		                    p->decoder.number_of_frame_buffers,
		                    0);

	if (p->semaphore_can_get_audio < 0) {
		mkv_play_close(p, 0, pspType);
		return("mkv_play_open: sceKernelCreateSema failed on semaphore_can_get_audio");
	}

	p->semaphore_can_put_audio =
		sceKernelCreateSema("can_put_audio",
		                    0,
		                    p->decoder.number_of_frame_buffers,
		                    p->decoder.number_of_frame_buffers,
		                    0);

	if (p->semaphore_can_put_audio < 0) {
		mkv_play_close(p, 0, pspType);
		return("mkv_play_open: sceKernelCreateSema failed on semaphore_can_put_audio");
	}

	p->output_thread =
		ppa_thread_create(PPA_THREAD_AUDIO_OUTPUT,
		                  "mkv_audio",
		                  mkv_output_thread,
		                  0x10000,
		                  0);

	if (p->output_thread < 0) {
		mkv_play_close(p, 0, pspType);
		return("mkv_play_open: sceKernelCreateThread failed on output_thread");
	}

	p->show_thread = ppa_session_audio_only() ? -1 :
		ppa_thread_create(PPA_THREAD_VIDEO_PRESENT,
		                  "mkv_video",
		                  mkv_show_thread,
		                  0x10000,
		                  ppa_video_pipeline_vfpu_required() ?
		                      PSP_THREAD_ATTR_VFPU : 0U);

	if (!ppa_session_audio_only() && p->show_thread < 0) {
		mkv_play_close(p, 0, pspType);
		return("mkv_play_open: sceKernelCreateThread failed on show_thread");
	}

	{
		unsigned int demux_thread_attributes =
			ppu_audio_stream_thread_attributes(p->decoder.audio_stream);

		p->demux_thread =
			ppa_thread_create(PPA_THREAD_DEMUX_DECODE,
			                  "mkv_decode",
			                  mkv_demux_thread,
			                  0x10000,
			                  demux_thread_attributes);
	}

	if (p->demux_thread < 0) {
		mkv_play_close(p, 0, pspType);
		return("mkv_play_open: sceKernelCreateThread failed on demux_thread");
	}

	p->return_request = 0;
	p->return_result  = 0;

	p->paused = 0;
	p->seek   = 0;
	p->absolute_seek_ms = -1;
	p->chapter_skip_cursor = -1;
	p->chapter_skip_deadline_us = 0;
	p->seek_session_deadline_us = 0;
	p->seek_transactions = 0U;
	p->seek_failures = 0U;
	p->trickplay_frame_counter = 0U;
	p->trickplay_settle_pending = 0U;
	p->h264x_trickplay_saved_mode = p->h264x_playback_mode;
	ppa_video_pipeline_set_trickplay_frame_drop(0);
	ppa_seek_controller_reset((struct ppa_seek_controller *)&p->seek_controller);

	p->current_timestamp = 0;

	p->audio_stream     = 0;
	p->audio_channel    = 0;
	p->volume_boost     = 3;
	p->aspect_ratio     = 0;
	p->zoom             = 100;
	p->luminosity_boost = 0;
	p->show_interface   = 0;
	p->interface_hide_deadline_us = 0;
	ppa_overlay_reset();
	p->loop             = 0;
	p->resume_pos       = 0;
	p->last_keyframe_pos = 0;
	p->subtitle_format =
		(gu_font_any_face_has_unicode_charmap() ? 1 : 0);
	p->subtitle_fontcolor = 0;
	p->subtitle_bordercolor = 0;

	memcpy(p->hash, movie->movie_hash, 16);
	p->persistent_state_ready = 1;

	if (usePos && !ppa_session_audio_only()) {
		mkv_stat_load(p);
	}
	if (!ppa_session_audio_only())
		movie_stat_active_begin(p->hash, p->movie_file, &p->current_timestamp);
	if (ppa_video_pipeline_extreme_battery_saver_enabled())
		p->luminosity_boost = 0U;

	/*
	* Preferred subtitle language is a global user setting.
	* Apply it after movie-stat restore so old per-movie subtitle state does
	* not override the current configuration preference.
	*/
	if (p->subtitle_count > 0) {
		p->subtitle = mkv_play_choose_preferred_subtitle(p);
	}
	else {
		p->subtitle = 0;
	}

	/*
	 * Load the fallback font set for the initially selected subtitle.
	 *
	 * Without this call, the first/default subtitle track keeps whatever
	 * fallback fonts were already loaded until the user toggles subtitles.
	 * The X + L toggle path also calls this after changing p->subtitle.
	 */
	if (!ppa_session_audio_only()) mkv_play_refresh_subtitle_font_set(p);
	p->subtitle_font_reload_pending = 0;	

	return(0);
}
