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
#include "mp4_decode.h"
#include "common/ppa_playback_session.h"
#include <stdint.h>
#include <limits.h>
#include "me_boot_start.h"
#include "codec_prx.h"
#include "audio_util.h"
#include "psp1k_frame_buffer.h"
#include "../common/ppa_packet_pool.h"
#include "../common/ppa_time.h"
#include "../common/ppa_cache.h"
#include "../common/ppa_memory.h"
#include "../media/VideoPipeline.h"

static void clear_mp4_timestamp_queue(int *queue,
                                      unsigned int *queue_size,
                                      unsigned int queue_max);
static int in_mp4_timestamp_queue(struct mp4_decode_struct *p, int timestamp,
                                  const struct h264x_picture_identity *identity) {
	unsigned int i;
	int *queue = p->timestamp_queue;
	unsigned int *queue_size = &p->timestamp_queue_size;
	if (*queue_size >= mp4_timestamp_queue_max)
		return 0;
	for (i = 0U; i < *queue_size; i++) {
		if (timestamp < queue[i])
			break;
	}
	if (i < *queue_size) {
		memmove(queue + i + 1U, queue + i, (*queue_size - i) * sizeof(queue[0]));
		memmove(p->h264x_present_queue + i + 1U, p->h264x_present_queue + i,
		        (*queue_size - i) * sizeof(p->h264x_present_queue[0]));
	}
	queue[i] = timestamp;
	memset(&p->h264x_present_queue[i], 0, sizeof(p->h264x_present_queue[i]));
	if (identity != 0) p->h264x_present_queue[i] = *identity;
	p->h264x_present_queue[i].timestamp = timestamp;
	*queue_size += 1;
	return 1;
}

static void mp4_decode_render_output(struct mp4_decode_struct *p,
                                     unsigned int aspect_ratio,
                                     unsigned int zoom,
                                     unsigned int luminosity_boost,
                                     unsigned int show_interface,
                                     unsigned int show_subtitle,
                                     unsigned int subtitle_format,
                                     unsigned int frame_number)
{
	void *frame;
	int direct_scanout;
	int pipeline_active;

	if (p == 0)
		return;

	frame = p->video_frame_buffers[p->current_video_buffer_number];
	direct_scanout = ppa_gu_direct_scanout_enabled();
	pipeline_active = !direct_scanout && ppa_video_pipeline_enabled();

	if (pipeline_active) {
		ppa_gu_draw_video_only(aspect_ratio, zoom, luminosity_boost,
		                       subtitle_format, frame_number, frame);
	}
	else {
		ppa_gu_draw(aspect_ratio, zoom, luminosity_boost,
		            show_interface, show_subtitle, subtitle_format,
		            frame_number, frame);
	}

	p->output_video_frame_buffers[p->current_video_buffer_number].data = frame;
	ppa_gu_wait();

	/* Direct eDRAM scanout is GE -> display ownership only.  The ordinary
	 * no-filter main-RAM path is also GE -> display, so neither path needs a
	 * per-frame CPU cache traversal.  Cache ownership changes only when the
	 * opt-in CPU post-process implementation is compiled and active. */
	if (pipeline_active) {
		unsigned int frame_bytes =
			(unsigned int)p->output_texture_width * 272U * 4U;
		ppa_cache_device_wrote_cpu_will_read(frame, frame_bytes);
		ppa_video_pipeline_process(frame,
		                           (unsigned int)p->output_texture_width,
		                           p->reader.file.video_width,
		                           p->reader.file.video_height,
		                           aspect_ratio, zoom, frame_number);
		ppa_gu_overlay_cpu_frame(show_interface, show_subtitle,
		                         subtitle_format, frame_number, frame,
		                         p->output_texture_width);
		ppa_cache_wb_range(frame, frame_bytes);
	}
}

static unsigned int mp4_decode_next_epoch(unsigned int epoch)
{
	epoch++;
	return epoch == 0U ? 1U : epoch;
}

static void mp4_h264x_select_compat(struct mp4_decode_struct *p)
{
	struct h264x_compat_selection selection;

	if (p == 0)
		return;
	p->h264x_compat_active = 0U;
	p->h264x_compat_original_refs = 0U;
	p->h264x_compat_effective_refs = 0U;
	p->h264x_compat_disable_weights = 0U;
	p->h264x_compat_flatten_brefs = 0U;
	p->h264x_compat_force_level_idc = 0U;
	p->h264x_compat_retry_stage = 0U;
	p->h264x_compat_first_idr_retry_attempted = 0U;
	p->h264x_compat_mpeg_mode =
		(p->reader.file.video_width > 480 ||
		 p->reader.file.video_height > 272) ? 5U : 4U;

	if (!p->h264x.sps.valid || !p->h264x.pps.valid)
		return;

	h264x_compat_select(&p->h264x, &selection);
	p->h264x_compat_original_refs = selection.original_refs;
	p->h264x_compat_effective_refs = selection.effective_refs;
	p->h264x_compat_disable_weights = selection.disable_weights;
	p->h264x_compat_active = selection.flags ? 1U : 0U;
	if (p->h264x_compat_active)
		p->h264x_compat_mpeg_mode = 5U;

}

static int mp4_h264x_boot_type(const struct mp4_decode_struct *p,
	                           unsigned int mpeg_mode)
{
	if (mpeg_mode == 5U)
		return 1;
	if (p != 0 && p->reader.file.avc_profile == 0x42)
		return 4;
	return 3;
}

static char *mp4_h264x_open_context(struct mp4_decode_struct *p)
{
	void *compat_sps = 0;
	void *compat_pps = 0;
	unsigned int compat_sps_size = 0U;
	unsigned int compat_pps_size = 0U;
	unsigned int sps_capacity;
	unsigned int pps_capacity;
	unsigned int mpeg_mode;
	char *result;

	if (p == 0)
		return "mp4_h264x_open_context: null decoder";

	/* Extreme battery saver deliberately bypasses compatibility selection, so
	 * choose the native Sony decoder context directly from the coded geometry.
	 * Without this override the constructor's legacy mode-4 default would be
	 * reused for otherwise-admissible 720x480 streams. */
	if (ppa_video_pipeline_extreme_battery_saver_enabled())
		mpeg_mode = (p->reader.file.video_width > 480 ||
		             p->reader.file.video_height > 272) ? 5U : 4U;
	else
		mpeg_mode = p->h264x_compat_mpeg_mode;
	if (mpeg_mode != 4U && mpeg_mode != 5U)
		mpeg_mode = (p->reader.file.video_width > 480 ||
		             p->reader.file.video_height > 272) ? 5U : 4U;

	if (!p->h264x_compat_active ||
	    (p->h264x_compat_retry_stage == 0U && !p->h264x_compat_disable_weights)) {
		if (me_boot_start(mp4_h264x_boot_type(p, mpeg_mode)) != 0)
			return "mp4 h264x: Sony ME boot failed (see boot log)";
		return mp4_avc_open(&p->avc,
		                    p->reader.file.avc_profile,
		                    (int)mpeg_mode,
		                    p->reader.file.avc_sps,
		                    p->reader.file.avc_sps_size,
		                    p->reader.file.avc_pps,
		                    p->reader.file.avc_pps_size,
		                    p->reader.file.avc_nal_prefix_size);
	}

	sps_capacity = p->reader.file.avc_sps_size + 64U;
	pps_capacity = p->reader.file.avc_pps_size + 64U;
	compat_sps = malloc_64(sps_capacity);
	compat_pps = malloc_64(pps_capacity);
	if (compat_sps == 0 || compat_pps == 0) {
		if (compat_sps != 0) free_64(compat_sps);
		if (compat_pps != 0) free_64(compat_pps);
		return "mp4 h264x: parameter-set allocation failed";
	}

	if (!h264x_rewrite_parameter_sets_compat(
	        p->reader.file.avc_sps,
	        p->reader.file.avc_sps_size,
	        p->reader.file.avc_pps,
	        p->reader.file.avc_pps_size,
	        p->h264x_compat_effective_refs,
	        p->h264x_compat_force_level_idc,
	        p->h264x_compat_disable_weights,
	        compat_sps, sps_capacity, &compat_sps_size,
	        compat_pps, pps_capacity, &compat_pps_size)) {
		free_64(compat_sps);
		free_64(compat_pps);
		return "mp4 h264x: parameter-set rewrite failed";
	}

	/* Complete fallible parameter preparation before changing firmware mode. */
	if (me_boot_start(mp4_h264x_boot_type(p, mpeg_mode)) != 0) {
		free_64(compat_sps);
		free_64(compat_pps);
		return "mp4 h264x: Sony ME boot failed (see boot log)";
	}
	result = mp4_avc_open(&p->avc,
	                      p->reader.file.avc_profile,
	                      (int)mpeg_mode,
	                      compat_sps, (int)compat_sps_size,
	                      compat_pps, (int)compat_pps_size,
	                      p->reader.file.avc_nal_prefix_size);
	free_64(compat_sps);
	free_64(compat_pps);
	return result;
}

static char *mp4_h264x_retry_first_idr(struct mp4_decode_struct *p,
	                                  const void *data,
	                                  unsigned int size,
	                                  int *pic_num)
{
	unsigned int stage;
	char *result = "mp4 h264x: compatibility ladder exhausted";

	if (p == 0 || data == 0 || size == 0U || pic_num == 0)
		return "mp4 h264x: invalid first-IDR retry";
	if (!p->h264x_compat_active || !p->h264x.current_au.is_idr)
		return result;

	p->h264x_compat_first_idr_retry_attempted++;
	for (stage = 1U; stage <= 6U; ++stage) {
		p->h264x_compat_retry_stage = stage;
		if (stage == 1U) {
			/* refs=3 first: preserve the anchor/reference-B working set. */
		}
		else if (stage == 2U) {
			if (p->h264x_compat_effective_refs <= 2U) continue;
			p->h264x_compat_effective_refs = 2U;
		}
		else if (stage == 3U) {
			if (p->h264x_compat_mpeg_mode != 5U) continue;
			p->h264x_compat_mpeg_mode = 4U;
		}
		else if (stage == 4U) {
			if (p->h264x_compat_disable_weights ||
			    (!p->h264x.pps.weighted_pred_flag && !p->h264x.pps.weighted_bipred_idc)) continue;
			p->h264x_compat_disable_weights = 1U;
		}
		else if (stage == 5U) {
			if (p->h264x.sps.level_idc == 30U) continue;
			p->h264x_compat_force_level_idc = 30U;
		}
		else {
			p->h264x_compat_effective_refs = 1U;
			p->h264x_compat_flatten_brefs = 1U;
			p->h264x_compat_force_level_idc = 30U;
		}

		mp4_avc_close(&p->avc);
		result = mp4_h264x_open_context(p);
		if (result != 0 && p->h264x_compat_mpeg_mode == 5U) {
			p->h264x_compat_mpeg_mode = 4U;
			result = mp4_h264x_open_context(p);
		}
		if (result != 0)
			continue;
		ppa_cache_cpu_wrote_me_will_read(data, size);
		*pic_num = 0;
		result = mp4_avc_get(&p->avc, 3, (void *)data, (int)size,
		                     ppa_gu_rgb_buffer, pic_num);
		if (result == 0) {
			return 0;
		}
	}
	return result;
}

static void mp4_decode_begin_discontinuity(struct mp4_decode_struct *p)
{
	if (p == 0)
		return;
	p->is_eof = 0;
	p->avc_first_packet_done = 0;
	p->output_epoch = mp4_decode_next_epoch(p->output_epoch);
	clear_mp4_timestamp_queue(p->timestamp_queue,
	                          &p->timestamp_queue_size,
	                          mp4_timestamp_queue_max);
	h264x_probe_reset_stream(&p->h264x);
	memset(p->h264x_present_queue, 0, sizeof(p->h264x_present_queue));
	p->h264x_output_remap.pending_slot0 = 0U;
	p->h264x_deep_b_detected = p->h264x_anchor_valid = p->h264x_anchor_poc_lsb = 0U;
}

static int out_mp4_timestamp_queue(struct mp4_decode_struct *p, int *timestamp) {
	int *queue = p->timestamp_queue;
	unsigned int *queue_size = &p->timestamp_queue_size;
	unsigned int i;
	if (!timestamp) return 0;
	*timestamp = -1;
	if (*queue_size == 0 || *queue_size > mp4_timestamp_queue_max)
		return 0;

	*timestamp = queue[0];

	for (i = 1; i < *queue_size; i++) {
		queue[i - 1] = queue[i];
		p->h264x_present_queue[i - 1] = p->h264x_present_queue[i];
	}

	queue[i - 1] = -1;
	memset(&p->h264x_present_queue[i - 1], 0, sizeof(p->h264x_present_queue[0]));
	*queue_size -= 1;

	return 1;
}

static void clear_mp4_timestamp_queue(int* queue,
                                      unsigned int* queue_size,
                                      unsigned int queue_max) {
	int i;

	for (i = 0; i < queue_max; i++) {
		queue[i] = -1;
	}

	*queue_size = 0;

}

void mp4_decode_safe_constructor(struct mp4_decode_struct *p) {
	p->audio_decoder = -1;
	p->audio_stream = 0;
	p->audio_stream_selected = 0;
	mp4_read_safe_constructor(&p->reader);

	mp4_avc_safe_constructor(&p->avc);

	int i = 0;
	for (; i < mp4_maximum_frame_buffers; i++) {
		p->video_frame_buffers[i] = 0;
		p->audio_frame_buffers[i] = 0;
	}
	
	memset(p->output_audio_frame_buffers, 0,
	       sizeof(p->output_audio_frame_buffers));
	memset(p->output_video_frame_buffers, 0,
	       sizeof(p->output_video_frame_buffers));
	clear_mp4_timestamp_queue(p->timestamp_queue, &p->timestamp_queue_size, mp4_timestamp_queue_max);
	memset(p->h264x_present_queue, 0, sizeof(p->h264x_present_queue));
	memset(&p->h264x_output_remap, 0, sizeof(p->h264x_output_remap));
	p->h264x_deep_b_detected = p->h264x_anchor_valid = p->h264x_anchor_poc_lsb = 0U;
	p->last_audio_timestamp = 0;
	p->last_video_timestamp = 0;
	p->is_eof = 0;
	p->avc_first_packet_done = 0;
	p->output_epoch = 1U;
	h264x_probe_init(&p->h264x);
	p->h264x_rewrite_buffer = 0;
	p->h264x_rewrite_capacity = 0U;
	p->h264x_compat_active = 0U;
	p->h264x_compat_original_refs = 0U;
	p->h264x_compat_effective_refs = 0U;
	p->h264x_compat_disable_weights = 0U;
	p->h264x_compat_flatten_brefs = 0U;
	p->h264x_compat_force_level_idc = 0U;
	p->h264x_compat_mpeg_mode = 4U;
	p->h264x_compat_retry_stage = 0U;
	p->h264x_compat_first_idr_retry_attempted = 0U;

}

void mp4_decode_close(struct mp4_decode_struct *p, int pspType) {
	h264x_two_picture_release(&p->h264x_output_remap);
    ppu_audio_stream_close(p->audio_stream);
    p->audio_stream = 0;
	media_codecs_close(&p->avc, &p->audio_decoder);
	mp4_read_close(&p->reader);

	if (p->h264x_rewrite_buffer != 0) {
		free_64(p->h264x_rewrite_buffer);
		p->h264x_rewrite_buffer = 0;
		p->h264x_rewrite_capacity = 0U;
	}
	clear_reset_framebuffer();
	h264x_probe_close(&p->h264x);
	
	int i = 0;
	for (; i < mp4_maximum_frame_buffers; i++) {
		if (p->audio_frame_buffers[i] != 0) 
			free_64(p->audio_frame_buffers[i]);
	}
	
	mp4_decode_safe_constructor(p);
}

char *mp4_decode_open(struct mp4_decode_struct *p,
                      char *s,
                      int pspType,
                      int tvAspectRatio,
                      int tvWidth,
                      int tvHeight,
                      int videoMode) {
	char *result;
	unsigned int display_width;
	unsigned int display_height;
	uint64_t duration;

	mp4_decode_safe_constructor(p);

	cpu_clock_set_maximum();

	result = mp4_read_open(&p->reader, s);

	if (result != 0) {
		mp4_decode_close(p, pspType);
		return(result);
	}

	p->audio_frame_size = (p->reader.file.audio_resample_scale << 1) << p->reader.file.audio_stereo;
	if (!ppa_session_audio_only()) {
	ppa_video_pipeline_set_avc_sps(p->reader.file.avc_sps,
	                               p->reader.file.avc_sps_size);
	if (h264x_probe_open_parameter_sets(
	        &p->h264x,
	        p->reader.file.avc_sps, p->reader.file.avc_sps_size,
	        p->reader.file.avc_pps, p->reader.file.avc_pps_size,
	        p->reader.file.avc_nal_prefix_size,
	        p->reader.file.video_width,
	        p->reader.file.video_height) == 0 &&
	    !ppa_video_pipeline_extreme_battery_saver_enabled()) {
		mp4_h264x_select_compat(p);
	}
	mp4_read_set_video_reorder_requirement(&p->reader,
	                                       p->h264x.sps.num_ref_frames);
	/* The shallow B-pyramid bridge is required even when SPS/PPS are native.
	 * Reserve one bounded compressed workspace at open, never per frame. */
	if (!ppa_video_pipeline_extreme_battery_saver_enabled() &&
	    PPA_H264X_REWRITE_ENABLED && PPA_H264X_PRE_REWRITE &&
	    p->reader.file.maximum_video_sample_size != 0U &&
	    p->reader.file.maximum_video_sample_size <= 16U * 1024U * 1024U) {
		p->h264x_rewrite_capacity =
			p->reader.file.maximum_video_sample_size + 512U;
		p->h264x_rewrite_buffer =
			malloc_64(p->h264x_rewrite_capacity);
		if (p->h264x_rewrite_buffer == 0)
			p->h264x_rewrite_capacity = 0U;
	}

	p->video_format = p->reader.file.video_type;

	result = mp4_h264x_open_context(p);
	if (result != 0 && p->h264x_compat_active && p->h264x_compat_mpeg_mode == 5U) {
		/* Same initial mode-4 fallback as MKV, including weights-off opens. */
		p->h264x_compat_mpeg_mode = 4U;
		result = mp4_h264x_open_context(p);
	}

	if (result != 0) {
		mp4_decode_close(p, pspType);
		return(result);
	}

	display_width = p->reader.file.video_width;
	display_height = p->reader.file.video_height;

	if (display_width == 720 && display_height == 480) {
		display_width = 853;
	}

	aspect_ratio_struct_init(p->reader.file.video_width,
	                         p->reader.file.video_height,
	                         display_width,
	                         display_height,
	                         pspType,
	                         tvAspectRatio,
	                         tvWidth,
	                         tvHeight,
	                         videoMode);

	}

    p->audio_stream = ppu_audio_stream_open(&p->reader.file.audio_formats[0]);
    if (!p->audio_stream) {
        mp4_decode_close(p, pspType);
        return "audio: decoder or resampler initialization failed";
    }
    p->audio_decoder = audio_decoder_is_open() ? 0 : -1;

	cpu_clock_set_minimum();

	{
		int i = 0;

		if (ppa_session_audio_only()) {
			p->number_of_frame_buffers = 4; /* audio ring, no video producer */
			p->output_texture_width = ppa_memory_fixed_region_reserved() ? 768 : 512;
			p->video_frame_size = 0;
			p->video_frame_buffers[0] = ppa_memory_fixed_region_reserved() ?
			    ppa_memory_extended_frame_surface(0) : psp1k_get_frame_buffer(0);
			if (!p->video_frame_buffers[0]) {
				mp4_decode_close(p, pspType);
				return "audio-only: missing static display surface";
			}
		}
		else if (ppa_gu_direct_scanout_enabled()) {
			p->output_texture_width = PPA_GU_DIRECT_SCANOUT_PITCH;
			p->number_of_frame_buffers = PPA_GU_DIRECT_SCANOUT_COUNT;
			p->video_frame_size = PPA_GU_DIRECT_SCANOUT_BYTES;

			for (i = 0; i < p->number_of_frame_buffers; i++) {
				p->video_frame_buffers[i] =
					ppa_gu_direct_scanout_surface((unsigned int)i);
				if (p->video_frame_buffers[i] == 0) {
					mp4_decode_close(p, pspType);
					return("mp4_decode_open: invalid direct scanout surface");
				}
			}
		}
		else if (m33IsTVOutSupported(pspType) &&
		         ppa_memory_fixed_region_reserved()) {
			p->output_texture_width = 768;
			p->number_of_frame_buffers = 8;
			p->video_frame_size = PPA_MEMORY_EXTENDED_FRAME_BYTES;

			for (i = 0; i < p->number_of_frame_buffers; i++) {
				p->video_frame_buffers[i] =
					ppa_memory_extended_frame_surface((unsigned int)i);
				if (p->video_frame_buffers[i] == 0) {
					mp4_decode_close(p, pspType);
					return("mp4_decode_open: invalid extended frame surface");
				}
			}
		}
		else {
			p->output_texture_width = 512;
			p->number_of_frame_buffers = 4;
			p->video_frame_size = 557056;

			for (i = 0; i < p->number_of_frame_buffers; i++) {
				p->video_frame_buffers[i] = psp1k_get_frame_buffer(i);
			}
		}
	}

	{
		int i = 0;

		for (i = 0; i < p->number_of_frame_buffers; i++) {
			/* Output-only ring: decode/SRC scratch belongs to audio_stream. */

			p->audio_frame_buffers[i] = malloc_64(p->audio_frame_size);

			if (p->audio_frame_buffers[i] == 0) {
				mp4_decode_close(p, pspType);
				return("mp4_decode_open: malloc_64 failed on audio_frame_buffers");
			}

			memset(p->audio_frame_buffers[i], 0, p->audio_frame_size);
		}
	}

	/* TV interlace uses two separated field regions, with unused rows
	 * between them. Clear every surface once, including that padding; normal
	 * frame rendering must not expose old memory when a later ring slot swaps. */
	if (videoMode != 0) {
		unsigned int surface_count = ppa_session_audio_only() ? 1U :
		    (unsigned int)p->number_of_frame_buffers;
		unsigned int surface_index;
		for (surface_index = 0; surface_index < surface_count; ++surface_index) {
			if (ppa_gu_clear_frame_blocking(p->video_frame_buffers[surface_index],
			        (unsigned int)p->output_texture_width,
			        ppa_gu_scanout_width(), ppa_gu_scanout_storage_height()) < 0) {
				mp4_decode_close(p, pspType);
				return "mp4_decode_open: TV surface clear failed";
			}
		}
	}

	sceDisplayWaitVblankStart();
	if (!ppa_session_audio_only() && ppa_gu_direct_scanout_enabled()) {
		/* Do not let the display scan the first eDRAM surface while the GE is
		 * rendering the first frame into it. */
		void *bootstrap = ppa_memory_extended_frame_surface(0);
		if (bootstrap == 0) {
			mp4_decode_close(p, pspType);
			return("mp4_decode_open: missing direct-scanout bootstrap surface");
		}
		if (ppa_gu_clear_frame_blocking(bootstrap, 768U, 480U, 272U) < 0) {
			mp4_decode_close(p, pspType);
			return("mp4_decode_open: GE bootstrap clear failed");
		}
		sceDisplaySetFrameBuf(bootstrap, 768,
		                      PSP_DISPLAY_PIXEL_FORMAT_8888,
		                      PSP_DISPLAY_SETBUF_IMMEDIATE);
	}
	else {
		if (ppa_gu_clear_frame_blocking(p->video_frame_buffers[0],
		                                (unsigned int)p->output_texture_width,
		                                ppa_gu_scanout_width(),
		                                ppa_gu_scanout_storage_height()) < 0) {
			mp4_decode_close(p, pspType);
			return("mp4_decode_open: GE initial framebuffer clear failed");
		}
		sceDisplaySetFrameBuf(p->video_frame_buffers[0],
		                      p->output_texture_width,
		                      PSP_DISPLAY_PIXEL_FORMAT_8888,
		                      PSP_DISPLAY_SETBUF_IMMEDIATE);
	}

	p->current_video_buffer_number = 0;
	p->current_audio_buffer_number = 0;

	duration = 1000LL;
	duration *= p->reader.file.video_scale;
	duration /= p->reader.file.video_rate;
	p->video_frame_duration = duration;

	duration = 1000LL;
	duration *= p->reader.file.audio_resample_scale;
	duration /= p->reader.file.audio_rate;
	p->audio_frame_duration = duration;

	ppa_gu_init_previous_values();

	return(0);
}

int   mp4_decode_is_eof(struct mp4_decode_struct *p) {
	return (p->is_eof);
}

void  mp4_decode_reset(struct mp4_decode_struct *p) {
	(void)mp4_decode_seek(p, 0, 0);
}

char *mp4_decode_seek(struct mp4_decode_struct *p, int timestamp, int last_timestamp) {
	char *result;

	if (p == 0)
		return "mp4_decode_seek: null decoder";
	result = mp4_read_seek(&p->reader, timestamp, last_timestamp);
	if (result != 0)
		return result;

    if (!ppu_audio_stream_reset(p->audio_stream, timestamp <= 0))
        return "audio: seek reset failed";

	/* Sony's AVC object retains hidden DPB/CABAC state. A discontinuity must
	 * start from a fresh context even when the target sample is a valid IDR. */
	mp4_avc_close(&p->avc);
	result = mp4_h264x_open_context(p);
	if (result != 0)
		return result;
	mp4_decode_begin_discontinuity(p);
	p->last_audio_timestamp = timestamp;
	p->last_video_timestamp = timestamp;
	return 0;
}

char *mp4_decode_keyframe_forward(struct mp4_decode_struct *p, int keyframes) {
	return mp4_read_keyframe_forward(&p->reader, keyframes);
}

char *mp4_decode_keyframe_backward(struct mp4_decode_struct *p, int keyframes) {
	return mp4_read_keyframe_backward(&p->reader, keyframes);
}

static char *mp4_audio_packet(void *reader, unsigned int track,
                                  struct ppa_media_packet *packet)
{
    char *error = mp4_read_get_audio((struct mp4_read_struct *)reader, track, packet);
    if (error && !strcmp(error, "mp4_read_fill_buffer: eof")) return PPU_AUDIO_EOF;
    if (error && !strcmp(error, MP4_READ_AUDIO_VIDEO_BACKPRESSURE)) return PPU_AUDIO_AGAIN;
    return error;
}

char *mp4_decode_get_audio(struct mp4_decode_struct *p,
                           unsigned int audio_stream, int audio_channel,
                           int decode_audio, unsigned int volume_boost,
                           unsigned int *codec_us)
{
    char *error;
    unsigned int work_us = 0, post_started, post_us;
    int timestamp = 0;
    int16_t *output = p->audio_frame_buffers[p->current_audio_buffer_number];
    (void)volume_boost;
    if (codec_us) *codec_us = 0;
    if (audio_stream >= (unsigned int)p->reader.file.audio_tracks)
        return "audio: invalid track selection";
    if (audio_stream != p->audio_stream_selected) {
        ppu_audio_stream_close(p->audio_stream);
        p->audio_stream = ppu_audio_stream_open(&p->reader.file.audio_formats[audio_stream]);
        p->audio_decoder = audio_decoder_is_open() ? 0 : -1;
        if (!p->audio_stream || !ppu_audio_stream_reset(p->audio_stream, 0))
            return "audio: track reconfiguration failed";
        p->audio_stream_selected = audio_stream;
    }
    error = ppu_audio_stream_read(p->audio_stream, mp4_audio_packet, &p->reader,
                                  audio_stream, output, &timestamp, &work_us);
    post_started = sceKernelGetSystemTimeLow();
    if (error) {
        if (!strcmp(error, PPU_AUDIO_AGAIN)) {
            error = (ppa_session_audio_only() || p->is_eof) ? PPU_AUDIO_AGAIN : MP4_READ_AUDIO_VIDEO_BACKPRESSURE;
            goto report;
        }
        if (strcmp(error, PPU_AUDIO_EOF) || ppa_session_audio_only() || p->is_eof) goto report;
        ppu_audio_stream_silence(p->audio_stream, output, &timestamp);
    }
    if (!decode_audio) memset(output, 0, PPU_AUDIO_BLOCK * 4);
    else {
        pcm_normalize(output, PPU_AUDIO_BLOCK * 2);
        pcm_select_channel(output, PPU_AUDIO_BLOCK * 2, audio_channel);
    }
    p->output_audio_frame_buffers[p->current_audio_buffer_number].data = output;
    p->output_audio_frame_buffers[p->current_audio_buffer_number].timestamp = timestamp;
    p->output_audio_frame_buffers[p->current_audio_buffer_number].epoch = p->output_epoch;
    p->last_audio_timestamp = timestamp;
    p->current_audio_buffer_number =
        (p->current_audio_buffer_number + 1) % p->number_of_frame_buffers;
    error = 0;
report:
    /* Include gain/channel conversion, including retry work, exactly once.
     * Demux/I/O waits remain excluded from this scalable-cost observation. */
    post_us = sceKernelGetSystemTimeLow() - post_started;
    work_us = post_us > ~0U - work_us ? ~0U : work_us + post_us;
    if (!ppa_session_audio_only()) cpu_clock_auto_on_audio_resample_us(work_us);
    if (codec_us) *codec_us = work_us;
    return error;
}

char *mp4_decode_get_cached_video(struct mp4_decode_struct *p,
                                  unsigned int pic_num,
                                  unsigned int audio_stream,
                                  unsigned int volume_boost,
                                  unsigned int aspect_ratio,
                                  unsigned int zoom,
                                  unsigned int luminosity_boost,
                                  unsigned int show_interface,
                                  unsigned int show_subtitle,
                                  unsigned int subtitle_format,
                                  unsigned int loop) {
	char *result;

	uint64_t t_auto_decode = cpu_clock_auto_now_us();
	unsigned int pitch = p->reader.file.video_width > 480U ? 768U : 512U;
	unsigned int height = (p->reader.file.video_height + 15U) & ~15U;
	int restored = h264x_two_picture_restore(&p->h264x_output_remap,
	    ppa_gu_rgb_buffer, pitch, height, pic_num);
	if (restored < 0) return "mp4 h264x: cached-picture restore failed";
	result = restored ? 0 : mp4_avc_get_cache(&p->avc, ppa_gu_rgb_buffer, pic_num);
	cpu_clock_auto_on_decode_us((unsigned int)(cpu_clock_auto_now_us() - t_auto_decode),
	                            p->video_frame_duration);

	if (result != 0) {
		return(result);
	}

	int timestamp;
	if (!out_mp4_timestamp_queue(p, &timestamp))
		return "mp4_decode_get_cached_video: presentation queue invariant failed";
	unsigned int tmp = ppa_timestamp_ms_to_frame(timestamp < 0 ? 0U : (unsigned)timestamp,
	    p->reader.file.video_rate, p->reader.file.video_scale);

	if (show_interface == 1) {
		draw_interface(
			p->reader.file.video_scale,
			p->reader.file.video_rate,
			p->reader.file.number_of_video_frames,
			(unsigned int)tmp,
			aspect_ratio,
			zoom,
			luminosity_boost,
			audio_stream,
			volume_boost,
			loop);
	}

	mp4_decode_render_output(p, aspect_ratio, zoom,
	                         luminosity_boost, show_interface,
	                         show_subtitle, subtitle_format,
	                         (unsigned int)tmp);
	p->output_video_frame_buffers[p->current_video_buffer_number].timestamp =
		timestamp;
	p->output_video_frame_buffers[p->current_video_buffer_number].epoch =
		p->output_epoch;

	p->last_video_timestamp = timestamp;

	p->current_video_buffer_number =
		(p->current_video_buffer_number + 1) % p->number_of_frame_buffers;

	return(0);
}

char *mp4_decode_get_video(struct mp4_decode_struct *p,
                           unsigned int audio_stream,
                           unsigned int volume_boost,
                           unsigned int aspect_ratio,
                           unsigned int zoom,
                           unsigned int luminosity_boost,
                           unsigned int show_interface,
                           unsigned int show_subtitle,
                           unsigned int subtitle_format,
                           unsigned int loop,
                           int *pic_num) {
	char *result;
	struct mp4_read_output_struct v_packet;
	int battery_saver_active;
	memset(&v_packet, 0, sizeof(struct mp4_read_output_struct));
	battery_saver_active = ppa_video_pipeline_extreme_battery_saver_enabled();

	if (p->is_eof) {
		*pic_num = 1;

		if (!in_mp4_timestamp_queue(p,
		        p->last_video_timestamp + p->video_frame_duration, 0))
			return "mp4_decode_get_video: timestamp queue full";
	}
	else {
		result = mp4_read_get_video(&p->reader, &v_packet);

		if (result != 0) {
			if (strcmp(result, "mp4_read_fill_buffer: eof") == 0) {
				*pic_num = 1;
				p->is_eof = 1;

				if (!in_mp4_timestamp_queue(p,
				        p->last_video_timestamp + p->video_frame_duration, 0))
					return "mp4_decode_get_video: timestamp queue full";
			}
			else {
				return(result);
			}
		}
		else {
			void *decode_data = v_packet.data;
			unsigned int decode_size = v_packet.size;
			unsigned int rewritten_size = 0U;
			unsigned int rewrite_flags = 0U;
			struct h264x_picture_identity identity;
			memset(&identity, 0, sizeof(identity));
			int weights_rewrite_required;
			int avc_mode = p->avc_first_packet_done ? 0 : 3;
			/* Include mandatory per-P weight conversion in the one governor
			 * sample; its CPU work must not disappear from the clock budget. */
			uint64_t t_auto_decode = cpu_clock_auto_now_us();

			if (!battery_saver_active) {
				h264x_probe_before_decode(&p->h264x, v_packet.data,
				                          v_packet.size, v_packet.timestamp,
				                          avc_mode);
				if (p->h264x.current_au.is_idr)
					avc_mode = 3;
				identity.valid = p->h264x.current_au.valid && !p->h264x.current_au.parse_error;
				identity.poc_lsb = p->h264x.current_au.pic_order_cnt_lsb;
				identity.is_b = p->h264x.current_au.is_b;
				identity.is_reference_b = p->h264x.current_au.is_reference_b;
				/* Match MKV's observed >B3 list-preservation boundary. This does
				 * not introduce its paused deep-DPB translator into MP4. */
				if (identity.valid && p->h264x.sps.pic_order_cnt_type == 0U &&
				    (p->h264x.current_au.is_idr || p->h264x.current_au.is_p)) {
					unsigned int bits = p->h264x.sps.log2_max_pic_order_cnt_lsb_minus4 + 4U;
					if (p->h264x.current_au.is_idr) p->h264x_anchor_valid = 0U;
					if (p->h264x_anchor_valid && bits >= 4U && bits <= 16U) {
						unsigned int delta = (identity.poc_lsb - p->h264x_anchor_poc_lsb) & ((1U << bits) - 1U);
						if (delta > PPA_H264X_DEEP_B_POC_DELTA_THRESHOLD)
							p->h264x_deep_b_detected = 1U;
					}
					p->h264x_anchor_valid = 1U;
					p->h264x_anchor_poc_lsb = identity.poc_lsb;
				}
			}

			weights_rewrite_required = !battery_saver_active &&
				p->h264x_compat_active && p->h264x_compat_disable_weights &&
				h264x_rewrite_weights_required(&p->h264x);
			if (!battery_saver_active && PPA_H264X_REWRITE_ENABLED && PPA_H264X_PRE_REWRITE &&
			    p->h264x_rewrite_buffer != 0 &&
			    v_packet.size <= p->h264x_rewrite_capacity) {
				rewrite_flags = h264x_ref_bridge_flags(&p->h264x,
				    p->h264x_compat_effective_refs, p->h264x_compat_flatten_brefs,
				    PPA_H264X_DEEP_PRESERVE_RPLM && p->h264x_deep_b_detected,
				    weights_rewrite_required);

				if (rewrite_flags != 0U && !p->h264x.current_au.uses_8x8_transform &&
				    h264x_rewrite_ref_mgmt_select(
				        &p->h264x, v_packet.data, v_packet.size,
				        p->h264x_rewrite_buffer,
				        p->h264x_rewrite_capacity, &rewritten_size,
				        rewrite_flags)) {
					decode_data = p->h264x_rewrite_buffer;
					decode_size = rewritten_size;
				}
			}

			if (weights_rewrite_required && decode_data == v_packet.data) {
				mp4_read_release_output(&v_packet);
				return "h264x: cannot safely remove explicit weights";
			}

			ppa_cache_cpu_wrote_me_will_read(decode_data, decode_size);
			if (!in_mp4_timestamp_queue(p, v_packet.timestamp, &identity)) {
				mp4_read_release_output(&v_packet);
				return "mp4_decode_get_video: timestamp queue full";
			}

			{
				unsigned int elapsed_auto_decode;

				result = mp4_avc_get(&p->avc, avc_mode,
				                     decode_data, (int)decode_size,
				                     ppa_gu_rgb_buffer, pic_num);
				if (result != 0 && !p->avc_first_packet_done &&
				    p->h264x.current_au.is_idr &&
				    p->h264x_compat_active) {
					result = mp4_h264x_retry_first_idr(
						p, v_packet.data, v_packet.size, pic_num);
				}
				/* Include the guarded GE copy/extra CSC in this AU's governor
				 * sample. The next cached call accounts for the saved slot. */
				if (result == 0 && !battery_saver_active && *pic_num == 2 &&
				    p->timestamp_queue_size >= 2U &&
				    h264x_two_picture_shape(&p->h264x_present_queue[0],
				        &p->h264x_present_queue[1], p->video_frame_duration)) {
					unsigned int pitch = p->reader.file.video_width > 480U ? 768U : 512U;
					unsigned int height = (p->reader.file.video_height + 15U) & ~15U;
					if (h264x_two_picture_prepare(&p->h264x_output_remap, &p->avc,
					        ppa_gu_rgb_buffer, pitch, height) < 0)
						result = "mp4 h264x: two-picture recovery failed";
				}

				elapsed_auto_decode =
					(unsigned int)(cpu_clock_auto_now_us() - t_auto_decode);
				ppa_session_note_compat(p->h264x_compat_original_refs,
                        p->h264x_compat_effective_refs, p->h264x_compat_active,
                        p->h264x_compat_disable_weights);
                cpu_clock_auto_on_decode_us(elapsed_auto_decode,
				                            p->video_frame_duration);
			}

			if (result == 0)
				p->avc_first_packet_done = 1;

			if (v_packet.data)
				mp4_read_release_output(&v_packet);

			if (result != 0) {
				return(result);
			}
		}
	}

	*pic_num = *pic_num - 1;

	if (*pic_num >= 0) {
		int timestamp;
		if (!out_mp4_timestamp_queue(p, &timestamp))
			return "mp4_decode_get_video: presentation queue invariant failed";
		unsigned int tmp = ppa_timestamp_ms_to_frame(timestamp < 0 ? 0U : (unsigned)timestamp,
		    p->reader.file.video_rate, p->reader.file.video_scale);

		if (show_interface == 1) {
			draw_interface(
				p->reader.file.video_scale,
				p->reader.file.video_rate,
				p->reader.file.number_of_video_frames,
				(unsigned int)tmp,
				aspect_ratio,
				zoom,
				luminosity_boost,
				audio_stream,
				volume_boost,
				loop);
		}

		mp4_decode_render_output(p, aspect_ratio, zoom,
		                         luminosity_boost, show_interface,
		                         show_subtitle, subtitle_format,
		                         (unsigned int)tmp);
		p->output_video_frame_buffers[p->current_video_buffer_number].timestamp =
			timestamp;
		p->output_video_frame_buffers[p->current_video_buffer_number].epoch =
			p->output_epoch;

		p->last_video_timestamp = timestamp;

		p->current_video_buffer_number =
			(p->current_video_buffer_number + 1) % p->number_of_frame_buffers;

	}

	return(0);
}
