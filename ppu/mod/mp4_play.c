#include "codec_prx.h"
#include "../common/ppa_process.h"
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
#include "../common/ppa_video_limits.h"
#include "../common/ctrl.h"
#include <stdarg.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include "mp4_play.h"
#include "common/ppa_playback_session.h"
#include "media/PlaybackUi.h"
#include "audio_util.h"
#include "audio_vfpu.h"
#include "../common/ppa_wait.h"
#include "../common/ppa_frame_sink.h"
#include "../common/ppa_hardware_profile.h"
#include "../common/ppa_playback_control.h"
#include "../common/ppa_thread_policy.h"
#include "../common/ppa_bus_manager.h"
#include "../common/ppa_privileged_bridge.h"
#include "../common/ppa_chapter.h"
#include "../common/ppa_overlay.h"
#include "cpu_clock.h"
#include "../media/VideoPipeline.h"

#ifndef PPA_MP4_SHUTDOWN_WAIT_US
#define PPA_MP4_SHUTDOWN_WAIT_US 1500000U
#endif
#ifndef PPA_MP4_SHUTDOWN_FORCE_WAIT_US
#define PPA_MP4_SHUTDOWN_FORCE_WAIT_US 250000U
#endif
#ifndef PPA_MP4_PIPELINE_STALL_MS
#define PPA_MP4_PIPELINE_STALL_MS 8000U
#endif

static void mp4_play_note_progress(volatile struct mp4_play_struct *p)
{
	if (p != 0)
		p->pipeline_progress_ms =
			(uint32_t)(cpu_clock_auto_now_us() / 1000ULL);
}

static int mp4_battery_video_quick_probe(struct mp4_decode_struct *decoder)
{
	unsigned int picture_count = 0U;
	unsigned int b_run = 0U;
	unsigned int i;
	int compatible = 1;

	if (decoder == 0)
		return 0;
	for (i = 0U; i < 32U; ++i) {
		struct mp4_read_output_struct packet;
		char *result;
		memset(&packet, 0, sizeof(packet));
		result = mp4_read_get_video(&decoder->reader, &packet);
		if (result != 0)
			break;
		h264x_probe_before_decode(&decoder->h264x, packet.data,
		                          packet.size, packet.timestamp,
		                          picture_count == 0U ? 3 : 0);
		if (decoder->h264x.current_au.parse_error ||
		    decoder->h264x.current_au.is_reference_b) {
			compatible = 0;
		}
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
		mp4_read_release_output(&packet);
		if (!compatible)
			break;
	}
	if (mp4_read_seek(&decoder->reader, 0, 0) != 0)
		compatible = 0;
	h264x_probe_reset_stream(&decoder->h264x);
	return compatible && picture_count != 0U;
}

static int mp4_extreme_battery_saver_compatible(
	struct mp4_decode_struct *decoder)
{
	const struct mp4_file_struct *file;
	const struct h264x_probe *probe;
	unsigned int profile;
	int audio_index;

	if (decoder == 0)
		return 0;
	file = &decoder->reader.file;
	probe = &decoder->h264x;
	if (file->video_width == 0U || file->video_height == 0U ||
	    file->video_width > PPA_VIDEO_MAX_CODED_WIDTH ||
	    file->video_height > PPA_VIDEO_MAX_CODED_HEIGHT)
		return 0;
	if (!probe->sps.valid || !probe->pps.valid || !probe->avcc_valid)
		return 0;
	profile = probe->sps.profile_idc != 0U ?
		probe->sps.profile_idc : file->avc_profile;
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
	if (file->video_stts_entry_count != 1U ||
	    file->video_stts_sample_duration == 0 ||
	    file->video_stts_sample_duration[0] == 0U)
		return 0;
	for (audio_index = 0; audio_index < file->audio_tracks; ++audio_index) {
		if (!file->audio_battery_cbr[audio_index])
			return 0;
	}
	return mp4_battery_video_quick_probe(decoder);
}

void mp4_play_safe_constructor(struct mp4_play_struct *p) {
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
	p->seek_transactions = 0U;
	p->seek_failures = 0U;
	p->pipeline_progress_ms = 0U;
	p->pipeline_watchdog_exits = 0U;
	ppa_seek_controller_reset(&p->seek_controller);
	p->audio_reserved = -1;

	p->semaphore_can_get_video   = -1;
	p->semaphore_can_put_video   = -1;
	p->semaphore_can_get_audio   = -1;
	p->semaphore_can_put_audio   = -1;

	p->output_thread = -1;
	p->show_thread   = -1;	
	p->demux_thread  = -1;
	
	mp4_decode_safe_constructor(&p->decoder);
}

void mp4_play_close(struct mp4_play_struct *p, int usePos, int pspType) {
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
		mp4_stat_save(p);

	mp4_decode_close(&p->decoder, pspType);

	unsigned int i = 0;
	for (i=0; i<p->subtitle_count; i++)
		subtitle_parse_close( &subtitle_parser[i] );
	
	mp4_play_safe_constructor(p);
}

static int mp4_wait(volatile struct mp4_play_struct *p,
                    SceUID s,
                    char *e) {
	return ppa_wait_sema_poll(&p->return_request,
	                              s,
	                              e,
	                              (char **)&p->return_result,
	                              PPA_SEMA_WAIT_TIMEOUT_US);
}

static int mp4_avsync_status(int audio_timestamp, int video_timestamp, int video_frame_duration) {

	// if video ahead of audio, do nothing
	if((int64_t)video_timestamp - audio_timestamp > 2LL * video_frame_duration)
		return 0;

	// if audio ahead of video, skip frame
	if((int64_t)audio_timestamp - video_timestamp > 2LL * video_frame_duration)
		return 2;

	return 1;
}

static void mp4_play_seek_epoch_arrived(volatile struct mp4_play_struct *p,
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
        ppa_seek_controller_reset(
            (struct ppa_seek_controller *)&p->seek_controller);
        ppa_video_pipeline_set_trickplay_frame_drop(0);
        ppa_playback_control_trickplay_leave(video_ts);
    }
    else if (ppa_seek_controller_active(
                 (const struct ppa_seek_controller *)&p->seek_controller)) {
        ppa_seek_controller_reset(
            (struct ppa_seek_controller *)&p->seek_controller);
        ppa_video_pipeline_set_trickplay_frame_drop(0);
        ppa_playback_control_trickplay_leave(video_ts);
    }
    else {
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

static int mp4_show_thread(SceSize input_length, void *input) {
	volatile struct mp4_play_struct *p = *((void **) input);

	(void)input_length;

	p->current_video_buffer_number = 0;

	int wait;
	int avsync_status = 1;
	struct ppa_frame_lease frame_lease;
	ppa_frame_lease_init(&frame_lease);
	int previous_video_ts = -1;
	unsigned int display_epoch = p->decoder.output_epoch;

	while (p->return_request == 0) {
		if (!ppa_session_pause_point(&p->return_request, &p->paused, PPA_SESSION_WORKER_VIDEO)) break;
		wait = mp4_wait(p,
		                p->semaphore_can_get_video,
		                "mp4_show_thread: sceKernelWaitSema failed on semaphore_can_get_video");

		if (wait == -1) {
			break;
		}
		else if (wait == 1) {
			int audio_ts;
			int video_ts;
			int frame_duration_ms;
			int trickplay_active;
			int intentional_trick_drop;
			int trickplay_signed_level;
			unsigned int trickplay_level;
			unsigned int frame_epoch;
			int seek_epoch_frame;
			int seek_arrival;

			mp4_play_note_progress(p);

			frame_epoch = p->decoder.output_video_frame_buffers[
			    p->current_video_buffer_number].epoch;
			if (frame_epoch != p->decoder.output_epoch) {
				unsigned int stale_epoch =
					p->decoder.output_video_frame_buffers[
						p->current_video_buffer_number].epoch;
				(void)stale_epoch;
				p->current_video_buffer_number =
					(p->current_video_buffer_number + 1) %
					p->decoder.number_of_frame_buffers;
				if (sceKernelSignalSema(p->semaphore_can_put_video, 1) < 0) {
					p->return_result =
						"mp4_show_thread: stale video release failed";
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
			}
			audio_ts = ppa_session_audio_time_ms(p->current_timestamp);
			video_ts =
				p->decoder.output_video_frame_buffers[p->current_video_buffer_number].timestamp;
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

			avsync_status = mp4_avsync_status(audio_ts,
			                                  video_ts,
			                                  frame_duration_ms);

			if (trickplay_active)
				avsync_status = intentional_trick_drop ? 2 : 1;
			if (seek_arrival) {
				avsync_status = 1;
				/* Ownership fences can display preroll frames and establish an
				 * earlier wall-clock anchor. Normal playback starts a new timeline
				 * here, with target audio still retained until this swap completes. */
				ppa_frame_sink_reset_timing();
			}

			if (avsync_status > 0) {
				int force_display_for_ownership;
				force_display_for_ownership =
					(avsync_status != 1 &&
					 frame_lease.retained_credits + 1U >=
					     (unsigned int)p->decoder.number_of_frame_buffers);

				if (avsync_status == 1 || force_display_for_ownership) {

					if (ppa_frame_sink_display_timed(
						p->decoder.output_video_frame_buffers[p->current_video_buffer_number].data,
						p->decoder.output_texture_width,
						PSP_DISPLAY_PIXEL_FORMAT_8888,
						PSP_DISPLAY_SETBUF_NEXTFRAME,
						video_ts, frame_duration_ms) < 0) {
						p->return_result = "mp4_show_thread: display transaction failed";
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
						mp4_play_seek_epoch_arrived(p, frame_epoch, video_ts);

					/* The completed vertical swap retires the previously displayed
					 * ring slot and every skipped slot behind it. Release them as one
					 * FIFO batch, then retain the newly displayed slot until a later
					 * successful swap. This prevents skipped frames from granting the
					 * decoder permission to overwrite the active scanout buffer. */
					int release_result = ppa_frame_lease_complete_swap(&frame_lease,
					    p->semaphore_can_put_video, p->decoder.output_video_frame_buffers[p->current_video_buffer_number].data,
					    frame_epoch, video_ts);
					if (release_result < 0) {
						p->return_result =
							"mp4_show_thread: display ownership release failed";
						p->return_request = 1;
						break;
					}

				}
				else {
					/* Intentional trick-play discard is not a missed playback
					 * deadline. Keep it out of governor/stutter feedback. */
					if (!intentional_trick_drop) {
						cpu_clock_auto_on_frame_skipped();
					}
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
					p->return_result  = "mp4_show_thread: sceKernelSignalSema failed on semaphore_can_get_video1";
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

static void mp4_post_seek_label(volatile struct mp4_play_struct *p, int level, int target_ms)
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

static int mp4_chapter_skip(volatile struct mp4_play_struct *p, int direction)
{
    const struct mp4_file_struct *file = (const struct mp4_file_struct *)&p->decoder.reader.file;
    int index;
    uint64_t now;
    int base_ms;
    if (file->chapter_count == 0) {
        ppa_overlay_post(PPA_OVERLAY_KEY_CHAPTER, PPA_OVERLAY_CENTER, 3000,
                         PPA_OVERLAY_UTF8 | PPA_OVERLAY_WHITE, ppa_playback_ui_text("chapter.none", "No chapters"));
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

static void mp4_input(volatile struct mp4_play_struct *p)
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
        if (p->subtitle_count) {
            p->subtitle = (p->subtitle + 1) % (p->subtitle_count + 1);
            ppa_playback_control_subtitle_change(p->subtitle);
            ppa_overlay_postf(PPA_OVERLAY_KEY_SUBTITLE, PPA_OVERLAY_TOP_RIGHT,
                1000, PPA_OVERLAY_UTF8, "[i]%s[/i]", p->subtitle ?
                subtitle_parser[p->subtitle - 1].filename :
                ppa_playback_ui_text("playback.no_subtitle", "No subtitle"));
        }
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
        mp4_post_seek_label(p, direction, target);
        break;
    }
    case PSP_CTRL_LTRIGGER: mp4_chapter_skip(p, -1); break;
    case PSP_CTRL_RTRIGGER: mp4_chapter_skip(p, 1); break;
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

static int mp4_output_thread(SceSize input_length, void *input) {
	volatile struct mp4_play_struct *p = *((void **) input);

	p->current_audio_buffer_number = 0;

	int wait;
	int first = 1;
	SceInt32 volume = 0;

	while (p->return_request == 0) {
		if (!ppa_session_pause_point(&p->return_request, &p->paused, PPA_SESSION_WORKER_AUDIO)) break;
		volatile struct mp4_decode_buffer_struct *current_buffer =
			&p->decoder.output_audio_frame_buffers[p->current_audio_buffer_number];

		wait = mp4_wait(p,
		                p->semaphore_can_get_audio,
		                "mp4_output_thread: sceKernelWaitSema failed on semaphore_can_get_audio");

		if (wait == -1) {
			break;
		}
		else if (wait == 1) {

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
						"mp4_output_thread: stale audio release failed";
					p->return_request = 1;
					break;
				}
				continue;
			}

			if (volume < PSP_AUDIO_VOLUME_MAX) {
				volume += PSP_AUDIO_VOLUME_MAX / 5;

				if (volume >= PSP_AUDIO_VOLUME_MAX)
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
			mp4_play_note_progress(p);
			p->current_audio_buffer_number =
				(p->current_audio_buffer_number + 1) %
				p->decoder.number_of_frame_buffers;

			if (first == 1) {
				first = 0;
			}
			else {
				if (sceKernelSignalSema(p->semaphore_can_put_audio, 1) < 0) {
					p->return_result  = "mp4_output_thread: sceKernelSignalSema failed on semaphore_can_put_audio";
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

static int mp4_play_begin_seek(volatile struct mp4_play_struct *p, int target_ms)
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

static int mp4_play_do_seek(volatile struct mp4_play_struct *p)
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
    origin = p->current_timestamp;
    p->absolute_seek_ms = -1;
    p->resume_pos = 0;
    p->seek = 0;
    p->last_keyframe_pos = origin;
    ppa_playback_control_seek(pos);
    if (!mp4_play_begin_seek(p, pos)) return 1;
    result = mp4_decode_seek((struct mp4_decode_struct *)&p->decoder, pos, origin);
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
    return 1;
}

void mp4_play_reset(volatile struct mp4_play_struct *p) {
    ppu_seek_request_reset((struct ppu_seek_request *)&p->seek_request);

	ppa_video_pipeline_set_trickplay_frame_drop(0);
	p->trickplay_settle_pending = 0U;
	p->seek_audio_target_queued = 0U;
	p->seek_in_flight = 0U;
	p->seek_in_flight_epoch = 0U;
	p->seek_in_flight_target_ms = 0;
	ppa_seek_controller_reset((struct ppa_seek_controller *)&p->seek_controller);
	ppa_playback_control_reset();

    if (mp4_play_begin_seek(p, 0)) {
        char *result = mp4_decode_seek((struct mp4_decode_struct *)&p->decoder, 0, 0);
        if (result) { p->return_result = result; p->return_request = 1; }
        else { p->current_timestamp = 0; p->seek_transactions++; }
    }
}

static int mp4_seek_audio_can_decode(volatile struct mp4_play_struct *p)
{
	return !p->seek_in_flight ||
	       p->seek_in_flight_epoch != p->decoder.output_epoch ||
	       !p->seek_audio_target_queued;
}

static char *mp4_audio_only_decode(void *owner, unsigned int *codec_us)
{
    volatile struct mp4_play_struct *p = owner;
    pcm_set_normalize_ratio(p->volume_boost);
    return mp4_decode_get_audio((struct mp4_decode_struct *)&p->decoder,
        p->audio_stream, p->audio_channel, 1, p->volume_boost, codec_us);
}
static int mp4_audio_only_eof(void *owner, const char *error)
{
    (void)owner;
    return strcmp(error, PPU_AUDIO_EOF) == 0;
}
static void mp4_audio_only_progress(void *owner)
{ mp4_play_note_progress((volatile struct mp4_play_struct *)owner); }

static void mp4_drain_audio_tail(volatile struct mp4_play_struct *p)
{
    struct ppa_audio_only_adapter a;
    volatile int complete = 0;
    memset(&a, 0, sizeof(a));
    a.owner = (void *)p; a.stop = &p->return_request; a.paused = &p->paused;
    a.eof = &complete; a.result = &p->return_result;
    a.can_put = p->semaphore_can_put_audio; a.can_get = p->semaphore_can_get_audio;
    a.decode = mp4_audio_only_decode; a.at_eof = mp4_audio_only_eof;
    a.progress = mp4_audio_only_progress; a.duration_ms = p->decoder.audio_frame_duration;
    a.drain_only = 1;
    ppa_session_run_audio_only(&a);
}

static int mp4_demux_thread(SceSize input_length, void *input) {
	volatile struct mp4_play_struct *p = *((void **) input);
	if (ppa_session_audio_only()) {
		struct ppa_audio_only_adapter a;
		memset(&a, 0, sizeof(a));
		a.owner = (void *)p;
		a.stop = &p->return_request; a.paused = &p->paused;
		a.eof = &p->decoder.is_eof; a.result = &p->return_result;
		a.can_put = p->semaphore_can_put_audio; a.can_get = p->semaphore_can_get_audio;
		a.decode = mp4_audio_only_decode; a.at_eof = mp4_audio_only_eof;
		a.progress = mp4_audio_only_progress; a.duration_ms = p->decoder.audio_frame_duration;
		return ppa_session_run_audio_only(&a);
	}


	int cached_video_frame = 0;

	while (p->return_request == 0 &&
	       !mp4_decode_is_eof((struct mp4_decode_struct *) &p->decoder)) {
		if (!ppa_session_pause_point(&p->return_request, &p->paused, PPA_SESSION_WORKER_DECODE)) break;
		if (ppa_process_exit_requested()) { p->return_request = 1; break; }
		int wait;
		char *result;

		wait = mp4_seek_audio_can_decode(p) ? mp4_wait(p,
		                p->semaphore_can_put_audio,
		                "mp4_play_start: sceKernelWaitSema failed on semaphore_can_put_audio") : 0;

		if (wait == -1) {
			break;
		}
		else if (wait == 1) {
			{
				struct ppa_bus_token decode_bus = ppa_bus_begin(PPA_BUS_DECODE);
				result = mp4_decode_get_audio((struct mp4_decode_struct *) &p->decoder,
			                              p->audio_stream,
			                              p->audio_channel,
			                              1,
			                              p->volume_boost, 0);
				ppa_bus_end(&decode_bus);
			}

			if (result != 0) {
				if (strcmp(result, MP4_READ_AUDIO_VIDEO_BACKPRESSURE) == 0) {
					if (sceKernelSignalSema(p->semaphore_can_put_audio, 1) < 0) {
						p->return_result =
							"mp4_play_start: sceKernelSignalSema failed on audio backpressure";
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
					p->return_result  = "mp4_play_start: sceKernelSignalSema failed on semaphore_can_get_audio";
					p->return_request = 1;
					break;
				}

				mp4_play_note_progress(p);
			}
		}

		wait = mp4_wait(p,
		                p->semaphore_can_put_video,
		                "mp4_play_start: sceKernelWaitSema failed on semaphore_can_put_video");

		if (wait == -1) {
			break;
		}
		else if (wait == 1) {
			if (cached_video_frame > 0) {
				{
					struct ppa_bus_token decode_bus = ppa_bus_begin(PPA_BUS_DECODE);
					result = mp4_decode_get_cached_video((struct mp4_decode_struct *) &p->decoder,
				                                     cached_video_frame,
				                                     p->audio_stream,
				                                     p->volume_boost,
				                                     p->aspect_ratio,
				                                     p->zoom,
				                                     p->luminosity_boost,
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
					p->return_result  = "mp4_play_start: sceKernelSignalSema failed on semaphore_can_get_video";
					p->return_request = 1;
					break;
				}

				mp4_play_note_progress(p);
			}
			else {
				{
					struct ppa_bus_token decode_bus = ppa_bus_begin(PPA_BUS_DECODE);
					result = mp4_decode_get_video((struct mp4_decode_struct *) &p->decoder,
				                              p->audio_stream,
				                              p->volume_boost,
				                              p->aspect_ratio,
				                              p->zoom,
				                              p->luminosity_boost,
				                              p->show_interface,
				                              p->subtitle,
				                              p->subtitle_format,
				                              p->loop,
				                              &cached_video_frame);
					ppa_bus_end(&decode_bus);
				}

				if (result != 0) {
					if (strcmp(result, MP4_READ_VIDEO_AUDIO_BACKPRESSURE) == 0) {
						if (sceKernelSignalSema(p->semaphore_can_put_video, 1) < 0) {
							p->return_result =
								"mp4_play_start: sceKernelSignalSema failed on video backpressure";
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
						p->return_result  = "mp4_play_start: sceKernelSignalSema failed on semaphore_can_get_video";
						p->return_request = 1;
						break;
					}

					mp4_play_note_progress(p);
				}
				else {
					if (sceKernelSignalSema(p->semaphore_can_put_video, 1) < 0) {
						p->return_result  = "mp4_play_start: sceKernelSignalSema failed on semaphore_can_put_video1";
						p->return_request = 1;
						break;
					}
				}
			}
		}

		if (!p->decoder.is_eof && p->decoder.reader.file.audio_up_sample == 0 &&
		    mp4_seek_audio_can_decode(p)) {
			wait = mp4_wait(p,
			                p->semaphore_can_put_audio,
			                "mp4_play_start: sceKernelWaitSema failed on semaphore_can_put_audio");

			if (wait == -1) {
				break;
			}
			else if (wait == 1) {
				{
					struct ppa_bus_token decode_bus = ppa_bus_begin(PPA_BUS_DECODE);
					result = mp4_decode_get_audio((struct mp4_decode_struct *) &p->decoder,
				                              p->audio_stream,
				                              p->audio_channel,
				                              1,
				                              p->volume_boost, 0);
					ppa_bus_end(&decode_bus);
				}

				if (result != 0) {
					if (strcmp(result, MP4_READ_AUDIO_VIDEO_BACKPRESSURE) == 0) {
						if (sceKernelSignalSema(p->semaphore_can_put_audio, 1) < 0) {
							p->return_result =
								"mp4_play_start: sceKernelSignalSema failed on audio backpressure";
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
						p->return_result  = "mp4_play_start: sceKernelSignalSema failed on semaphore_can_get_audio";
						p->return_request = 1;
						break;
					}

					mp4_play_note_progress(p);
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

		if (mp4_play_do_seek(p))
			cached_video_frame = 0;

		if (mp4_decode_is_eof((struct mp4_decode_struct *) &p->decoder)) {
            mp4_drain_audio_tail(p);
            if (p->return_request) break;
			if (p->loop == 1) {
				cached_video_frame = 0;
				mp4_play_reset(p);
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

char *mp4_play_start(volatile struct mp4_play_struct *p) {
    enum ctrl_context previous_context = ctrl_set_context(
        ppa_session_audio_only() ? CTRL_CONTEXT_AUDIO_ONLY : CTRL_CONTEXT_PLAYBACK);

	ppa_frame_sink_reset_timing();
	ppa_session_start();
	ppa_playback_ui_tick_audio_only();

    /* Persistent libsamplerate history belongs to the decode owner. The
     * legacy endpoint-based VFPU resampler cannot share that state. */
    ppa_audio_accel_session_begin(PPU_AUDIO_BLOCK, PPU_AUDIO_BLOCK, 0);
	mp4_play_note_progress(p);
	if (ppa_session_start_workers(p->output_thread, p->show_thread, p->demux_thread,
	                              sizeof(p), &p, &p->return_request) < 0) {
		ppa_audio_accel_session_end();
		ctrl_set_context(previous_context);
		return "playback: worker start failed";
	}


	while (p->return_request == 0 &&
	       (!mp4_decode_is_eof((struct mp4_decode_struct *) &p->decoder) ||
            (!ppa_session_audio_only() && !p->audio_tail_done))) {
		if (ppa_process_exit_requested()) { p->return_request = 1; break; }
		uint32_t now_ms;
		mp4_input(p);
		now_ms = (uint32_t)(cpu_clock_auto_now_us() / 1000ULL);
		if (p->return_request == 0 && p->paused == 0 &&
		    !ppa_seek_controller_active(
		        (const struct ppa_seek_controller *)&p->seek_controller) &&
		    p->trickplay_settle_pending == 0U &&
		    p->pipeline_progress_ms != 0U &&
		    (uint32_t)(now_ms - p->pipeline_progress_ms) >=
		        PPA_MP4_PIPELINE_STALL_MS) {
			p->pipeline_watchdog_exits++;
			p->return_result =
				"mp4 playback stalled in hardware decoder";
			p->return_request = 1;
		}
		/* Audio cadence belongs to its blocking output worker. Use a larger
		 * control yield with the LCD off, in addition to the input event wait. */
		sceKernelDelayThread(ppa_session_audio_only() ? 10000U : 1000U);
	}

	if (mp4_decode_is_eof((struct mp4_decode_struct *) &p->decoder)) {
		p->last_keyframe_pos = 0;
        /* The decode owner already drained PCM and hardware at natural EOF. */
	}
	p->return_request = 1;
	/* Queue waits poll with an 8 ms bound. Let them observe stop without
	 * inventing producer credits or consumer frames during a pending swap. */

	{
		int audio_join, show_join, decode_join;
		audio_join = ppa_wait_thread_end_safe(p->output_thread, "MP4", "audio",
		    PPA_MP4_SHUTDOWN_WAIT_US, PPA_MP4_SHUTDOWN_FORCE_WAIT_US);
		show_join = ppa_wait_thread_end_safe(p->show_thread, "MP4", "video_show",
		    PPA_MP4_SHUTDOWN_WAIT_US, PPA_MP4_SHUTDOWN_FORCE_WAIT_US);
		decode_join = ppa_wait_thread_end_safe(p->demux_thread, "MP4", "demux_decode",
		    PPA_MP4_SHUTDOWN_WAIT_US, PPA_MP4_SHUTDOWN_FORCE_WAIT_US);
		if (audio_join < 0 || show_join < 0 || decode_join < 0)
			ppa_wait_quarantine("MP4 worker did not return",
			    decode_join < 0 ? decode_join : (show_join < 0 ? show_join : audio_join));
	}
	ppa_audio_accel_session_end();

	ctrl_set_context(previous_context);
	return(p->return_result);
}

char *mp4_play_open(struct mp4_play_struct *p, struct movie_file_struct *movie, int usePos, int pspType, int tvAspectRatio, int tvWidth, int tvHeight, int videoMode) {
	mp4_play_safe_constructor(p);
	p->subtitle = 0;
	p->subtitle_count = 0;

	if (movie == 0 || movie->movie_file[0] == '\0')
		return "mp4_play_open: invalid movie path";

	memset(p->movie_file, 0, sizeof(p->movie_file));
	{
		size_t path_size = strlen(movie->movie_file);
		if (path_size >= sizeof(p->movie_file))
			path_size = sizeof(p->movie_file) - 1U;
		memcpy(p->movie_file, movie->movie_file, path_size);
		p->movie_file[path_size] = 0;
	}

	char *result = mp4_decode_open(&p->decoder, movie->movie_file, pspType, tvAspectRatio, tvWidth, tvHeight, videoMode);
	if (result != 0) {
		mp4_play_close(p, 0, pspType);
		return(result);
	}
	if (!ppa_session_audio_only() && ppa_video_pipeline_extreme_battery_saver_enabled() &&
	    !mp4_extreme_battery_saver_compatible(&p->decoder)) {
		mp4_play_close(p, 0, pspType);
		return PPA_BATTERY_SAVER_REJECTED;
	}
	if (ppa_video_pipeline_extreme_battery_saver_enabled()) {
		p->decoder.h264x.enabled = 0;
		p->decoder.h264x_compat_active = 0U;
	}
	
	if ( !ppa_session_audio_only() && p->decoder.reader.file.subtitle_tracks > 0 ) {
		int subtitle_track = 0;
		while(p->subtitle_count < MAX_SUBTITLES && subtitle_track < p->decoder.reader.file.subtitle_tracks) {
			struct subtitle_parse_struct *cur_parser = &subtitle_parser[p->subtitle_count];
			subtitle_parse_safe_constructor(cur_parser);
			snprintf(cur_parser->filename, sizeof(cur_parser->filename), "mp4 subtitle track(%d)", p->decoder.reader.file.subtitle_track_ids[subtitle_track]);
			subtitle_track++;
			
			cur_parser->p_sub_frame = (struct subtitle_frame_struct*)malloc_64( sizeof(struct subtitle_frame_struct) );
			if (cur_parser->p_sub_frame==0) {
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
		p->subtitle = 1;
	}

	if ( ppa_privileged_audio_set_frequency(p->decoder.reader.file.audio_rate) != 0) {
		mp4_play_close(p, 0, pspType);
		return("mp4_play_open: sceAudioSetFrequency failed");
	}
	p->audio_reserved = sceAudioChReserve(0, p->decoder.reader.file.audio_resample_scale, PSP_AUDIO_FORMAT_STEREO);
	if (p->audio_reserved < 0) {
		mp4_play_close(p, 0, pspType);
		return("mp4_play_open: sceAudioChReserve failed");
	}
	if (!ppa_session_audio_only()) {
	p->semaphore_can_get_video = sceKernelCreateSema("can_get_video", 0, 0, p->decoder.number_of_frame_buffers, 0);
	if (p->semaphore_can_get_video < 0) {
		mp4_play_close(p, 0, pspType);
		return("mp4_play_open: sceKernelCreateSema failed on semaphore_can_get_video");
	}

	p->semaphore_can_put_video = sceKernelCreateSema("can_put_video", 0, p->decoder.number_of_frame_buffers, p->decoder.number_of_frame_buffers, 0);
	if (p->semaphore_can_put_video < 0) {
		mp4_play_close(p, 0, pspType);
		return("mp4_play_open: sceKernelCreateSema failed on semaphore_can_put_video");
	}

	}

    if (!ppu_seek_request_init(&p->seek_request)) {
        mp4_play_close(p, 0, pspType);
        return "playback: could not create seek request lock";
    }

	p->semaphore_can_get_audio = sceKernelCreateSema("can_get_audio", 0, 0, p->decoder.number_of_frame_buffers, 0);
	if (p->semaphore_can_get_audio < 0) {
		mp4_play_close(p, 0, pspType);
		return("mp4_play_open: sceKernelCreateSema failed on semaphore_can_get_audio");
	}

	p->semaphore_can_put_audio = sceKernelCreateSema("can_put_audio", 0, p->decoder.number_of_frame_buffers, p->decoder.number_of_frame_buffers, 0);
	if (p->semaphore_can_put_audio < 0) {
		mp4_play_close(p, 0, pspType);
		return("mp4_play_open: sceKernelCreateSema failed on semaphore_can_put_audio");
	}

	//p->output_thread = sceKernelCreateThread("output", mp4_output_thread, 0x8, 0x10000, 0, 0);
	p->output_thread = ppa_thread_create(PPA_THREAD_AUDIO_OUTPUT, "mp4_audio",
	                                     mp4_output_thread, 0x10000, 0);
	if (p->output_thread < 0) {
		mp4_play_close(p, 0, pspType);
		return("mp4_play_open: sceKernelCreateThread failed on output_thread");
	}

	//p->show_thread = sceKernelCreateThread("show", mp4_show_thread, 0x8, 0x10000, 0, 0);
	p->show_thread = ppa_session_audio_only() ? -1 : ppa_thread_create(PPA_THREAD_VIDEO_PRESENT, "mp4_video",
	                                   mp4_show_thread, 0x10000,
	                                   ppa_video_pipeline_vfpu_required() ?
	                                       PSP_THREAD_ATTR_VFPU : 0U);
	if (!ppa_session_audio_only() && p->show_thread < 0) {
		mp4_play_close(p, 0, pspType);
		return("mp4_play_open: sceKernelCreateThread failed on show_thread");
	}
	
	p->demux_thread = ppa_thread_create(PPA_THREAD_DEMUX_DECODE, "mp4_decode",
	                                    mp4_demux_thread, 0x10000,
	                                    ppu_audio_stream_thread_attributes(p->decoder.audio_stream));
	if (p->demux_thread < 0) {
		mp4_play_close(p, 0, pspType);
		return("mp4_play_open: sceKernelCreateThread failed on demux_thread");
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
	p->pipeline_progress_ms = 0U;
	p->pipeline_watchdog_exits = 0U;
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
	p->resume_pos		= 0;
	p->last_keyframe_pos= 0;
	p->subtitle_format  = (((gufont_haveflags&GU_FONT_HAS_UNICODE_CHARMAP))?1:0);
	p->subtitle_fontcolor = 0;
	p->subtitle_bordercolor = 0;
	
	memcpy(p->hash, movie->movie_hash, 16);
	p->persistent_state_ready = 1;
	
	if (usePos) mp4_stat_load(p);
	if (!ppa_session_audio_only())
		movie_stat_active_begin(p->hash, p->movie_file, &p->current_timestamp);
	if (ppa_video_pipeline_extreme_battery_saver_enabled())
		p->luminosity_boost = 0U;
	return(0);
}
