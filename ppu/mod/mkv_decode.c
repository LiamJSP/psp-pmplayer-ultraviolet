/* mkv_decode.c
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
#include "../common/ctrl.h"
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <stdarg.h>
#include <pspiofilemgr.h>
#include <pspctrl.h>
#include "mkv_decode.h"
#include "h264x_compat.h"
#include "common/ppa_playback_session.h"
#include "me_boot_start.h"
#include "codec_prx.h"
#include "audio_util.h"
#include "psp1k_frame_buffer.h"
#include "../common/ppa_cache.h"
#include "../common/ppa_packet_pool.h"
#include "../common/ppa_time.h"
#include "../common/ppa_memory.h"
#include "cpu_clock.h"
#include "../media/VideoPipeline.h"

/* Several H.264X helpers are intentionally reachable only in selected build
 * profiles. Mark those static functions explicitly instead of weakening the
 * feature guards or accepting profile-dependent -Wunused-function noise. */
#if defined(__GNUC__)
#define PPA_MAYBE_UNUSED __attribute__((unused))
#else
#define PPA_MAYBE_UNUSED
#endif

#ifndef PPA_H264X_DEEP_PRESERVE_RPLM
#define PPA_H264X_DEEP_PRESERVE_RPLM 1
#endif
#ifndef PPA_H264X_DEEP_B_POC_DELTA_THRESHOLD
#define PPA_H264X_DEEP_B_POC_DELTA_THRESHOLD 8U
#endif

/*
 * 6.3.31 post-IDR reorder telemetry.
 *
 * Sony's AVC decoder legitimately returns zero immediately displayable
 * pictures for the first reference-B after some IDR resets, then releases a
 * later burst.  Keep a tiny process-local marker ring so the show thread can
 * identify the exact first damaged main-path B belonging to that reorder
 * bubble without changing mkv_decode_struct ABI or Sony's hidden DPB.
 */
#define MKV_H264X_REORDER_MARKER_COUNT 8U

struct mkv_h264x_reorder_marker_struct {
	volatile unsigned int valid;
	volatile unsigned int epoch;
	volatile unsigned int idr_au;
	volatile int idr_ts;
	volatile unsigned int zero_au;
	volatile int zero_ts;
	volatile unsigned int recovery_au;
	volatile int recovery_ts;
	volatile unsigned int recovery_pic_num;
};

static volatile struct mkv_decode_struct *mkv_h264x_reorder_owner;
static struct mkv_h264x_reorder_marker_struct
	mkv_h264x_reorder_markers[MKV_H264X_REORDER_MARKER_COUNT];
static volatile unsigned int mkv_h264x_reorder_epoch;
static volatile unsigned int mkv_h264x_reorder_marker_pos;
static volatile int mkv_h264x_reorder_last_pic_num;
static volatile unsigned int mkv_h264x_reorder_decode_calls;
static volatile unsigned int mkv_h264x_reorder_zero_calls;
static volatile unsigned int mkv_h264x_reorder_multi_calls;
static volatile unsigned int mkv_h264x_reorder_zero_refb_calls;
static volatile unsigned int mkv_h264x_reorder_recovery_calls;

/*
 * 6.3.40B stream-local deep-pyramid detector.  This deliberately uses only
 * already-parsed POC/timestamp metadata and never changes AU submission order.
 * The verified B3 path stays untouched until an anchor-to-anchor POC gap proves
 * that more than three B pictures are present.
 */
static volatile unsigned int mkv_h264x_deep_b_detected;
static volatile unsigned int mkv_h264x_deep_b_anchor_valid;
static volatile unsigned int mkv_h264x_deep_b_anchor_poc_lsb;
static volatile int mkv_h264x_deep_b_anchor_ts;
static volatile unsigned int mkv_h264x_deep_b_detect_au;

/* Dual-DPB list translator.  The source mirror executes the original MMCO
 * contract.  The Sony mirror executes the decoder-facing refs3 + stripped-MMCO
 * contract.  Modes can observe divergence or replace only RPLM syntax when
 * every intended source reference is still present in Sony's mirror. */
#define PPA_H264X_DUAL_MODE_OFF 0U
#define PPA_H264X_DUAL_MODE_APPLY_ALL 3U
#define PPA_H264X_DUAL_LIST_MAX 16U
static volatile unsigned int mkv_h264x_dual_mode;
static struct h264x_dpb mkv_h264x_source_dpb;
static struct h264x_dpb mkv_h264x_sony_dpb;
static unsigned int mkv_h264x_dual_ready;
static unsigned int mkv_h264x_dual_aus;
static unsigned int mkv_h264x_dual_divergent;
static unsigned int mkv_h264x_dual_equivalent;
static unsigned int mkv_h264x_dual_proven;
static unsigned int mkv_h264x_dual_impossible;
static unsigned int mkv_h264x_dual_plans;
static unsigned int mkv_h264x_dual_rewrites;
static unsigned int mkv_h264x_dual_rewrite_failures;

/* 6.3.43B full POC identity state. H.264 POC type 0 derives the current
 * MSB from the previous reference picture. Keeping the epoch and full value
 * beside each queued presentation record lets deep bursts be verified across
 * POC-LSB wrap without changing compressed AU order or Sony's hidden DPB. */
static volatile unsigned int mkv_h264x_poc_epoch;
static volatile unsigned int mkv_h264x_poc_prev_ref_valid;
static volatile unsigned int mkv_h264x_poc_prev_ref_lsb;
static volatile int mkv_h264x_poc_prev_ref_msb;

/* 6.3.43B keeps full-POC identity entirely private to this translation unit.
 * Do not add fields to mkv_h264x_present_risk_struct or mkv_decode_struct:
 * those types cross several player translation units and changing their size
 * can invalidate caller allocation/layout assumptions on older make trees. */
#define PPA_H264X_POC_IDENTITY_RING 64U
struct mkv_h264x_poc_identity_entry {
	unsigned int valid;
	int timestamp;
	unsigned int au_index;
	int full_poc;
	unsigned int poc_epoch;
};
static struct mkv_h264x_poc_identity_entry
	mkv_h264x_poc_identity_ring[PPA_H264X_POC_IDENTITY_RING];
static volatile unsigned int mkv_h264x_poc_identity_pos;

static void mkv_h264x_reorder_reset(struct mkv_decode_struct *p)
{
	unsigned int i;

	mkv_h264x_reorder_owner = p;
	mkv_h264x_reorder_epoch = 0U;
	mkv_h264x_reorder_marker_pos = 0U;
	mkv_h264x_reorder_last_pic_num = -1;
	mkv_h264x_reorder_decode_calls = 0U;
	mkv_h264x_reorder_zero_calls = 0U;
	mkv_h264x_reorder_multi_calls = 0U;
	mkv_h264x_reorder_zero_refb_calls = 0U;
	mkv_h264x_reorder_recovery_calls = 0U;
	mkv_h264x_deep_b_detected = 0U;
	mkv_h264x_deep_b_anchor_valid = 0U;
	mkv_h264x_deep_b_anchor_poc_lsb = 0U;
	mkv_h264x_deep_b_anchor_ts = -1;
	mkv_h264x_deep_b_detect_au = 0U;
	if (ppa_video_pipeline_extreme_battery_saver_enabled())
		mkv_h264x_dual_mode = PPA_H264X_DUAL_MODE_OFF;
	else
		mkv_h264x_dual_mode = PPA_H264X_DUAL_MODE_APPLY_ALL;
	h264x_dpb_init(&mkv_h264x_source_dpb);
	h264x_dpb_init(&mkv_h264x_sony_dpb);
	mkv_h264x_dual_ready = 0U;
	mkv_h264x_dual_aus = 0U;
	mkv_h264x_dual_divergent = 0U;
	mkv_h264x_dual_equivalent = 0U;
	mkv_h264x_dual_proven = 0U;
	mkv_h264x_dual_impossible = 0U;
	mkv_h264x_dual_plans = 0U;
	mkv_h264x_dual_rewrites = 0U;
	mkv_h264x_dual_rewrite_failures = 0U;
	mkv_h264x_poc_epoch = 0U;
	mkv_h264x_poc_prev_ref_valid = 0U;
	mkv_h264x_poc_prev_ref_lsb = 0U;
	mkv_h264x_poc_prev_ref_msb = 0;
	mkv_h264x_poc_identity_pos = 0U;
	for (i = 0U; i < PPA_H264X_POC_IDENTITY_RING; i++) {
		mkv_h264x_poc_identity_ring[i].valid = 0U;
		mkv_h264x_poc_identity_ring[i].timestamp = -1;
		mkv_h264x_poc_identity_ring[i].au_index = 0U;
		mkv_h264x_poc_identity_ring[i].full_poc = 0;
		mkv_h264x_poc_identity_ring[i].poc_epoch = 0U;
	}
	for (i = 0U; i < MKV_H264X_REORDER_MARKER_COUNT; i++) {
		mkv_h264x_reorder_markers[i].valid = 0U;
		mkv_h264x_reorder_markers[i].epoch = 0U;
		mkv_h264x_reorder_markers[i].idr_au = 0U;
		mkv_h264x_reorder_markers[i].idr_ts = -1;
		mkv_h264x_reorder_markers[i].zero_au = 0U;
		mkv_h264x_reorder_markers[i].zero_ts = -1;
		mkv_h264x_reorder_markers[i].recovery_au = 0U;
		mkv_h264x_reorder_markers[i].recovery_ts = -1;
		mkv_h264x_reorder_markers[i].recovery_pic_num = 0U;
	}
}

static unsigned int mkv_h264x_deep_b_poc_modulus(
	const struct mkv_decode_struct *p)
{
	unsigned int bits;

	if (p == 0 || !p->h264x.sps.valid ||
	    p->h264x.sps.pic_order_cnt_type != 0U)
		return 0U;
	bits = p->h264x.sps.log2_max_pic_order_cnt_lsb_minus4 + 4U;
	if (bits == 0U || bits >= 31U)
		return 0U;
	return 1U << bits;
}

static int mkv_h264x_extend_full_poc(struct mkv_decode_struct *p,
                                     const struct h264x_au_info *au,
                                     unsigned int *epoch_out)
{
	unsigned int modulus;
	unsigned int half;
	unsigned int lsb;
	int msb;
	int full_poc;

	if (epoch_out != 0)
		*epoch_out = mkv_h264x_poc_epoch;
	if (p == 0 || au == 0 || !au->valid || au->parse_error)
		return 0;
	modulus = mkv_h264x_deep_b_poc_modulus(p);
	if (modulus == 0U)
		return (int)au->pic_order_cnt_lsb;
	lsb = au->pic_order_cnt_lsb & (modulus - 1U);
	half = modulus >> 1U;

	if (au->nal_unit_type == 5U) {
		mkv_h264x_poc_epoch++;
		mkv_h264x_poc_prev_ref_valid = 1U;
		mkv_h264x_poc_prev_ref_lsb = lsb;
		mkv_h264x_poc_prev_ref_msb = 0;
		if (epoch_out != 0)
			*epoch_out = mkv_h264x_poc_epoch;
		return (int)lsb;
	}

	if (!mkv_h264x_poc_prev_ref_valid) {
		mkv_h264x_poc_prev_ref_valid = 1U;
		mkv_h264x_poc_prev_ref_lsb = lsb;
		mkv_h264x_poc_prev_ref_msb = 0;
	}
	msb = mkv_h264x_poc_prev_ref_msb;
	if (lsb < mkv_h264x_poc_prev_ref_lsb &&
	    mkv_h264x_poc_prev_ref_lsb - lsb >= half)
		msb += (int)modulus;
	else if (lsb > mkv_h264x_poc_prev_ref_lsb &&
	         lsb - mkv_h264x_poc_prev_ref_lsb > half)
		msb -= (int)modulus;
	full_poc = msb + (int)lsb;
	if (au->nal_ref_idc != 0U) {
		mkv_h264x_poc_prev_ref_lsb = lsb;
		mkv_h264x_poc_prev_ref_msb = msb;
	}
	if (epoch_out != 0)
		*epoch_out = mkv_h264x_poc_epoch;
	return full_poc;
}

static void mkv_h264x_poc_identity_store(struct mkv_decode_struct *p,
                                           const struct h264x_au_info *au,
                                           int timestamp)
{
	struct mkv_h264x_poc_identity_entry *e;
	unsigned int epoch;
	unsigned int slot;

	if (p == 0 || au == 0 || !au->valid || au->parse_error)
		return;
	epoch = 0U;
	slot = mkv_h264x_poc_identity_pos % PPA_H264X_POC_IDENTITY_RING;
	e = &mkv_h264x_poc_identity_ring[slot];
	e->valid = 1U;
	e->timestamp = timestamp;
	e->au_index = au->au_index;
	e->full_poc = mkv_h264x_extend_full_poc(p, au, &epoch);
	e->poc_epoch = epoch;
	mkv_h264x_poc_identity_pos++;
}

static int mkv_h264x_poc_identity_lookup(int timestamp,
                                          unsigned int au_index,
                                          int *full_poc_out,
                                          unsigned int *epoch_out)
{
	unsigned int seen;
	unsigned int pos;

	if (full_poc_out != 0)
		*full_poc_out = 0;
	if (epoch_out != 0)
		*epoch_out = 0U;
	pos = mkv_h264x_poc_identity_pos;
	for (seen = 0U; seen < PPA_H264X_POC_IDENTITY_RING && seen < pos; seen++) {
		unsigned int slot;
		const struct mkv_h264x_poc_identity_entry *e;
		slot = (pos - 1U - seen) % PPA_H264X_POC_IDENTITY_RING;
		e = &mkv_h264x_poc_identity_ring[slot];
		if (!e->valid)
			continue;
		if (e->timestamp != timestamp || e->au_index != au_index)
			continue;
		if (full_poc_out != 0)
			*full_poc_out = e->full_poc;
		if (epoch_out != 0)
			*epoch_out = e->poc_epoch;
		return 1;
	}
	return 0;
}

static void mkv_h264x_deep_b_observe(struct mkv_decode_struct *p, int timestamp)
{
	const struct h264x_au_info *au;
	unsigned int modulus;
	unsigned int poc_delta;
	int ts_delta;

	if (p == 0)
		return;
	if (mkv_h264x_reorder_owner != p)
		mkv_h264x_reorder_reset(p);
	au = &p->h264x.current_au;
	if (!au->valid || au->parse_error)
		return;

	/* Keep stream-level detection sticky, but restart anchor measurement at
	 * every IDR so POC-LSB wrap cannot create a false deep classification. */
	if (au->nal_unit_type == 5U) {
		mkv_h264x_deep_b_anchor_valid = 1U;
		mkv_h264x_deep_b_anchor_poc_lsb = au->pic_order_cnt_lsb;
		mkv_h264x_deep_b_anchor_ts = timestamp;
		return;
	}

	if (!au->is_p)
		return;
	if (!mkv_h264x_deep_b_anchor_valid) {
		mkv_h264x_deep_b_anchor_valid = 1U;
		mkv_h264x_deep_b_anchor_poc_lsb = au->pic_order_cnt_lsb;
		mkv_h264x_deep_b_anchor_ts = timestamp;
		return;
	}

	modulus = mkv_h264x_deep_b_poc_modulus(p);
	poc_delta = 0U;
	if (modulus != 0U)
		poc_delta = (au->pic_order_cnt_lsb + modulus -
		             mkv_h264x_deep_b_anchor_poc_lsb) % modulus;
	ts_delta = timestamp - mkv_h264x_deep_b_anchor_ts;
	(void)ts_delta;

#if PPA_H264X_DEEP_PRESERVE_RPLM
	if (!mkv_h264x_deep_b_detected &&
	    poc_delta > PPA_H264X_DEEP_B_POC_DELTA_THRESHOLD) {
		mkv_h264x_deep_b_detected = 1U;
		mkv_h264x_deep_b_detect_au = au->au_index;
	}
#endif

	mkv_h264x_deep_b_anchor_poc_lsb = au->pic_order_cnt_lsb;
	mkv_h264x_deep_b_anchor_ts = timestamp;
}

static int mkv_h264x_deep_b_preserve_rplm(void)
{
#if PPA_H264X_DEEP_PRESERVE_RPLM
	return mkv_h264x_deep_b_detected ? 1 : 0;
#else
	return 0;
#endif
}

unsigned int mkv_decode_h264x_deep_b_active(void)
{
	return mkv_h264x_deep_b_detected ? 1U : 0U;
}

void mkv_decode_h264x_dual_dpb_mode_set(unsigned int mode)
{
	if (mode != PPA_H264X_DUAL_MODE_APPLY_ALL)
		mode = PPA_H264X_DUAL_MODE_OFF;
	mkv_h264x_dual_mode = mode;
}

unsigned int mkv_decode_h264x_dual_dpb_mode_get(void)
{
	return mkv_h264x_dual_mode;
}

static int mkv_h264x_dual_same_ref(const struct h264x_dpb_ref *a,
                                    const struct h264x_dpb_ref *b)
{
	if (a == 0 || b == 0 || !a->valid || !b->valid)
		return 0;
	return a->au_index == b->au_index ? 1 : 0;
}

static void mkv_h264x_dual_swap_ref(struct h264x_dpb_ref *a,
                                    struct h264x_dpb_ref *b)
{
	struct h264x_dpb_ref t;
	t = *a;
	*a = *b;
	*b = t;
}

static unsigned int mkv_h264x_dual_build_default(
	const struct h264x_dpb *dpb, const struct h264x_au_info *au,
	unsigned int list1, struct h264x_dpb_ref *out)
{
	struct h264x_dpb_ref lower[PPA_H264X_DUAL_LIST_MAX];
	struct h264x_dpb_ref upper[PPA_H264X_DUAL_LIST_MAX];
	struct h264x_dpb_ref longs[PPA_H264X_DUAL_LIST_MAX];
	unsigned int nl;
	unsigned int nu;
	unsigned int ng;
	unsigned int i;
	unsigned int j;
	unsigned int n;
	int current_poc;

	nl = 0U;
	nu = 0U;
	ng = 0U;
	current_poc = (int)au->pic_order_cnt_lsb;
	for (i = 0U; i < H264X_DPB_MAX_REFS; i++) {
		const struct h264x_dpb_ref *r;
		r = &dpb->refs[i];
		if (!r->valid)
			continue;
		if (r->long_term) {
			if (ng < PPA_H264X_DUAL_LIST_MAX)
				longs[ng++] = *r;
		}
		else if (!au->is_b) {
			if (nl < PPA_H264X_DUAL_LIST_MAX) {
				lower[nl] = *r;
				/* FrameNumWrap is relative to the current picture, not the
				 * frame number at which this reference entered the DPB. */
				lower[nl].frame_num_wrap = (int)r->frame_num;
				if (r->frame_num > au->frame_num)
					lower[nl].frame_num_wrap -= (int)dpb->max_frame_num;
				++nl;
			}
		}
		else if (r->poc < current_poc) {
			if (nl < PPA_H264X_DUAL_LIST_MAX)
				lower[nl++] = *r;
		}
		else {
			if (nu < PPA_H264X_DUAL_LIST_MAX)
				upper[nu++] = *r;
		}
	}
	for (i = 0U; i < nl; i++) {
		for (j = i + 1U; j < nl; j++) {
			int swap;
			if (!au->is_b)
				swap = lower[j].frame_num_wrap > lower[i].frame_num_wrap;
			else
				swap = lower[j].poc > lower[i].poc;
			if (swap)
				mkv_h264x_dual_swap_ref(&lower[i], &lower[j]);
		}
	}
	for (i = 0U; i < nu; i++) {
		for (j = i + 1U; j < nu; j++) {
			int swap;
			swap = upper[j].poc < upper[i].poc;
			if (swap)
				mkv_h264x_dual_swap_ref(&upper[i], &upper[j]);
		}
	}
	for (i = 0U; i < ng; i++) {
		for (j = i + 1U; j < ng; j++) {
			if (longs[j].long_term_frame_idx < longs[i].long_term_frame_idx)
				mkv_h264x_dual_swap_ref(&longs[i], &longs[j]);
		}
	}
	n = 0U;
	if (au->is_b && list1) {
		for (i = 0U; i < nu && n < PPA_H264X_DUAL_LIST_MAX; i++) out[n++] = upper[i];
		for (i = 0U; i < nl && n < PPA_H264X_DUAL_LIST_MAX; i++) out[n++] = lower[i];
	}
	else {
		for (i = 0U; i < nl && n < PPA_H264X_DUAL_LIST_MAX; i++) out[n++] = lower[i];
		for (i = 0U; i < nu && n < PPA_H264X_DUAL_LIST_MAX; i++) out[n++] = upper[i];
	}
	for (i = 0U; i < ng && n < PPA_H264X_DUAL_LIST_MAX; i++) out[n++] = longs[i];
	return n;
}

static int mkv_h264x_dual_find_short(const struct h264x_dpb *dpb,
                                     unsigned int frame_num,
                                     struct h264x_dpb_ref *out)
{
	unsigned int i;
	for (i = 0U; i < H264X_DPB_MAX_REFS; i++) {
		if (dpb->refs[i].valid && dpb->refs[i].short_term &&
		    dpb->refs[i].frame_num == frame_num) {
			if (out) *out = dpb->refs[i];
			return 1;
		}
	}
	return 0;
}

static int mkv_h264x_dual_find_long(const struct h264x_dpb *dpb,
                                    unsigned int long_idx,
                                    struct h264x_dpb_ref *out)
{
	unsigned int i;
	for (i = 0U; i < H264X_DPB_MAX_REFS; i++) {
		if (dpb->refs[i].valid && dpb->refs[i].long_term &&
		    dpb->refs[i].long_term_frame_idx == long_idx) {
			if (out) *out = dpb->refs[i];
			return 1;
		}
	}
	return 0;
}

static int mkv_h264x_dual_find_au(const struct h264x_dpb *dpb,
                                  unsigned int au_index,
                                  struct h264x_dpb_ref *out)
{
	unsigned int i;
	for (i = 0U; i < H264X_DPB_MAX_REFS; i++) {
		if (dpb->refs[i].valid && dpb->refs[i].au_index == au_index) {
			if (out) *out = dpb->refs[i];
			return 1;
		}
	}
	return 0;
}

static void mkv_h264x_dual_insert_ref(struct h264x_dpb_ref *list,
                                      unsigned int *count,
                                      unsigned int pos,
                                      const struct h264x_dpb_ref *ref)
{
	unsigned int i;
	unsigned int n;
	if (list == 0 || count == 0 || ref == 0)
		return;
	n = *count;
	if (pos > n) pos = n;
	if (n < PPA_H264X_DUAL_LIST_MAX) n++;
	for (i = n; i > pos + 1U; i--)
		list[i - 1U] = list[i - 2U];
	list[pos] = *ref;
	for (i = pos + 1U; i < n; ) {
		if (mkv_h264x_dual_same_ref(&list[i], ref)) {
			unsigned int j;
			for (j = i + 1U; j < n; j++) list[j - 1U] = list[j];
			n--;
		}
		else i++;
	}
	*count = n;
}

static int mkv_h264x_dual_apply_source_ops(
	const struct h264x_dpb *dpb, const struct h264x_au_info *au,
	unsigned int list1, struct h264x_dpb_ref *list, unsigned int *count)
{
	const struct h264x_rplm_op *ops;
	unsigned int op_count;
	unsigned int max_frame_num;
	unsigned int pred;
	unsigned int pos;
	unsigned int i;

	ops = list1 ? au->rplm_l1 : au->rplm_l0;
	op_count = list1 ? au->rplm_l1_count : au->rplm_l0_count;
	if (op_count > H264X_MAX_RPLM_OPS || op_count > PPA_H264X_DUAL_LIST_MAX)
		return 0;
	if (op_count != (list1 ? au->ref_pic_list_modification_count_l1 :
	                         au->ref_pic_list_modification_count_l0))
		return 0;
	max_frame_num = dpb->max_frame_num ? dpb->max_frame_num : 16U;
	pred = au->frame_num;
	pos = 0U;
	for (i = 0U; i < op_count && i < H264X_MAX_RPLM_OPS; i++) {
		struct h264x_dpb_ref ref;
		unsigned int idc;
		int found;
		idc = ops[i].modification_of_pic_nums_idc;
		found = 0;
		if (idc == 0U || idc == 1U) {
			unsigned int delta;
			unsigned int target;
			delta = ops[i].abs_diff_pic_num_minus1 + 1U;
			if (delta == 0U || delta > max_frame_num)
				return 0;
			if (idc == 0U)
				target = (pred + max_frame_num - (delta % max_frame_num)) % max_frame_num;
			else
				target = (pred + delta) % max_frame_num;
			pred = target;
			found = mkv_h264x_dual_find_short(dpb, target, &ref);
		}
		else if (idc == 2U)
			found = mkv_h264x_dual_find_long(dpb, ops[i].long_term_pic_num, &ref);
		else
			return 0;
		if (!found)
			return 0;
		mkv_h264x_dual_insert_ref(list, count, pos, &ref);
		pos++;
	}
	return 1;
}

static unsigned int mkv_h264x_dual_active_count(
	const struct mkv_decode_struct *p, const struct h264x_au_info *au,
	unsigned int list1)
{
	unsigned int n;
	if (au->num_ref_idx_active_override_flag)
		n = list1 ? au->num_ref_idx_l1_active_minus1 :
		     au->num_ref_idx_l0_active_minus1;
	else
		n = list1 ? p->h264x.pps.num_ref_idx_l1_default_active_minus1 :
		     p->h264x.pps.num_ref_idx_l0_default_active_minus1;
	return n < PPA_H264X_DUAL_LIST_MAX ? n + 1U : 0U;
}

static int mkv_h264x_dual_make_ops(
	const struct h264x_dpb *sony, const struct h264x_au_info *au,
	const struct h264x_dpb_ref *desired, unsigned int desired_count,
	struct h264x_rplm_op *ops, unsigned int *op_count)
{
	unsigned int i;
	unsigned int pred;
	unsigned int max_frame_num;
	if (op_count) *op_count = 0U;
	pred = au->frame_num;
	max_frame_num = sony->max_frame_num ? sony->max_frame_num : 16U;
	for (i = 0U; i < desired_count; i++) {
		struct h264x_dpb_ref sony_ref;
		if (!mkv_h264x_dual_find_au(sony, desired[i].au_index, &sony_ref))
			return 0;
		memset(&ops[i], 0, sizeof(ops[i]));
		if (sony_ref.long_term) {
			ops[i].modification_of_pic_nums_idc = 2U;
			ops[i].long_term_pic_num = sony_ref.long_term_frame_idx;
		}
		else {
			unsigned int delta;
			delta = (pred + max_frame_num - sony_ref.frame_num) % max_frame_num;
			if (delta == 0U)
				delta = max_frame_num; /* Repeated reference after modulo wrap. */
			ops[i].modification_of_pic_nums_idc = 0U;
			ops[i].abs_diff_pic_num_minus1 = delta - 1U;
			pred = sony_ref.frame_num;
		}
	}
	if (op_count) *op_count = desired_count;
	return 1;
}

static int mkv_h264x_dual_list_equal(const struct h264x_dpb_ref *a,
                                     unsigned int na,
                                     const struct h264x_dpb_ref *b,
                                     unsigned int nb,
                                     unsigned int active)
{
	unsigned int i;
	if (active > na || active > nb)
		return 0;
	for (i = 0U; i < active; i++)
		if (!mkv_h264x_dual_same_ref(&a[i], &b[i]))
			return 0;
	return 1;
}

static void mkv_h264x_dual_prepare(struct mkv_decode_struct *p,
                                   struct h264x_rplm_override *plan)
{
	struct h264x_au_info source_au;
	struct h264x_au_info sony_au;
	struct h264x_sps_info sony_sps;
	struct h264x_dpb_ref source_l0[PPA_H264X_DUAL_LIST_MAX];
	struct h264x_dpb_ref source_l1[PPA_H264X_DUAL_LIST_MAX];
	struct h264x_dpb_ref sony_l0[PPA_H264X_DUAL_LIST_MAX];
	struct h264x_dpb_ref sony_l1[PPA_H264X_DUAL_LIST_MAX];
	unsigned int source_l0_n;
	unsigned int source_l1_n;
	unsigned int sony_l0_n;
	unsigned int sony_l1_n;
	unsigned int active_l0;
	unsigned int active_l1;
	unsigned int source_ok;
	unsigned int equivalent;
	unsigned int proven;
	unsigned int should_apply;

	if (plan) memset(plan, 0, sizeof(*plan));
	if (p == 0 || !p->h264x.current_au.valid || p->h264x.current_au.parse_error)
		return;
	source_au = p->h264x.current_au;
	sony_au = source_au;
	sony_sps = p->h264x.sps;
	if (p->h264x_compat_effective_refs != 0U)
		sony_sps.num_ref_frames = p->h264x_compat_effective_refs;
	sony_au.adaptive_ref_pic_marking_mode_flag = 0U;
	sony_au.mmco_count = 0U;
	sony_au.mmco_nonzero = 0U;
	sony_au.mmco_op_count = 0U;

	if (!mkv_h264x_dual_ready || source_au.is_idr) {
		h264x_dpb_reset(&mkv_h264x_source_dpb);
		h264x_dpb_reset(&mkv_h264x_sony_dpb);
		h264x_dpb_configure(&mkv_h264x_source_dpb, &p->h264x.sps);
		h264x_dpb_configure(&mkv_h264x_sony_dpb, &sony_sps);
		mkv_h264x_dual_ready = 1U;
	}

	if ((source_au.is_p || source_au.is_b) && mkv_h264x_deep_b_detected &&
	    mkv_h264x_dual_mode != PPA_H264X_DUAL_MODE_OFF) {
		source_l0_n = mkv_h264x_dual_build_default(&mkv_h264x_source_dpb, &source_au, 0U, source_l0);
		source_l1_n = source_au.is_b ? mkv_h264x_dual_build_default(&mkv_h264x_source_dpb, &source_au, 1U, source_l1) : 0U;
		sony_l0_n = mkv_h264x_dual_build_default(&mkv_h264x_sony_dpb, &source_au, 0U, sony_l0);
		sony_l1_n = source_au.is_b ? mkv_h264x_dual_build_default(&mkv_h264x_sony_dpb, &source_au, 1U, sony_l1) : 0U;
		/* B-list initialization swaps the first two L1 entries when the
		 * complete default lists are identical, before applying source RPLM. */
		if (source_au.is_b) {
			if (source_l1_n > 1U && source_l0_n == source_l1_n &&
			    mkv_h264x_dual_list_equal(source_l0, source_l0_n,
			                              source_l1, source_l1_n, source_l0_n))
				mkv_h264x_dual_swap_ref(&source_l1[0], &source_l1[1]);
			if (sony_l1_n > 1U && sony_l0_n == sony_l1_n &&
			    mkv_h264x_dual_list_equal(sony_l0, sony_l0_n,
			                              sony_l1, sony_l1_n, sony_l0_n))
				mkv_h264x_dual_swap_ref(&sony_l1[0], &sony_l1[1]);
		}
		source_ok = mkv_h264x_dual_apply_source_ops(&mkv_h264x_source_dpb, &source_au, 0U, source_l0, &source_l0_n) ? 1U : 0U;
		if (source_au.is_b && !mkv_h264x_dual_apply_source_ops(&mkv_h264x_source_dpb, &source_au, 1U, source_l1, &source_l1_n))
			source_ok = 0U;
		active_l0 = mkv_h264x_dual_active_count(p, &source_au, 0U);
		active_l1 = source_au.is_b ? mkv_h264x_dual_active_count(p, &source_au, 1U) : 0U;
		/* A missing active reference cannot be reconstructed from uninitialized
		 * array entries, nor can an unsupported active count be clipped. */
		if (active_l0 == 0U || active_l0 > source_l0_n ||
		    (source_au.is_b && (active_l1 == 0U || active_l1 > source_l1_n)))
			source_ok = 0U;
		equivalent = source_ok && mkv_h264x_dual_list_equal(source_l0, source_l0_n, sony_l0, sony_l0_n, active_l0);
		if (source_au.is_b)
			equivalent = equivalent && mkv_h264x_dual_list_equal(source_l1, source_l1_n, sony_l1, sony_l1_n, active_l1);
		/* 'proven' means representable in these software models only. The
		 * Sony mirror is predictive; it is not firmware DPB readback, and
		 * list identity alone does not establish temporal-direct fidelity. */
		proven = source_ok;
		if (proven && plan) {
			proven = mkv_h264x_dual_make_ops(&mkv_h264x_sony_dpb, &source_au, source_l0, active_l0, plan->l0, &plan->l0_count) ? 1U : 0U;
			if (proven && source_au.is_b)
				proven = mkv_h264x_dual_make_ops(&mkv_h264x_sony_dpb, &source_au, source_l1, active_l1, plan->l1, &plan->l1_count) ? 1U : 0U;
		}
		mkv_h264x_dual_aus++;
		if (equivalent) mkv_h264x_dual_equivalent++; else mkv_h264x_dual_divergent++;
		if (proven) mkv_h264x_dual_proven++; else mkv_h264x_dual_impossible++;
		/* Probe lists belong to the first slice; do not use them to choose
		 * overrides or default-list fallbacks for other slices in the AU. */
		should_apply = (mkv_h264x_dual_mode == PPA_H264X_DUAL_MODE_APPLY_ALL &&
		                source_au.vcl_count == 1U);
		if (plan && proven && !equivalent && should_apply) {
			plan->enabled = 1U;
			mkv_h264x_dual_plans++;
		}
		else if (plan && !proven && !equivalent && should_apply &&
		         (source_au.ref_pic_list_modification_flag_l0 ||
		          source_au.ref_pic_list_modification_flag_l1)) {
			/* Never submit a custom list that the reduced Sony DPB cannot
			 * represent. The attached stall ends exactly at this condition.
			 * Clearing RPLM makes the slice use Sony's valid default list. */
			plan->fallback_to_default = 1U;
		}
		
	}

	h264x_dpb_observe_au(&mkv_h264x_source_dpb, &p->h264x.sps, &source_au);
	h264x_dpb_observe_au(&mkv_h264x_sony_dpb, &sony_sps, &sony_au);
}

static struct mkv_h264x_reorder_marker_struct *
mkv_h264x_reorder_current_marker(void)
{
	unsigned int pos;

	if (mkv_h264x_reorder_epoch == 0U)
		return 0;
	pos = (mkv_h264x_reorder_marker_pos + MKV_H264X_REORDER_MARKER_COUNT - 1U) %
		MKV_H264X_REORDER_MARKER_COUNT;
	return &mkv_h264x_reorder_markers[pos];
}

static void mkv_h264x_reorder_note_decode(struct mkv_decode_struct *p,
                                          char *result,
                                          int pic_num,
                                          int timestamp)
{
	struct mkv_h264x_reorder_marker_struct *m;
	unsigned int au;
	unsigned int since_idr;
	int previous;

	if (p == 0 || result != 0)
		return;
	if (mkv_h264x_reorder_owner != p)
		mkv_h264x_reorder_reset(p);

	au = p->h264x.current_au.au_index;
	if (p->h264x.current_au.nal_unit_type == 5U) {
		unsigned int pos;
		mkv_h264x_reorder_epoch++;
		pos = mkv_h264x_reorder_marker_pos % MKV_H264X_REORDER_MARKER_COUNT;
		mkv_h264x_reorder_marker_pos =
			(mkv_h264x_reorder_marker_pos + 1U) % MKV_H264X_REORDER_MARKER_COUNT;
		m = &mkv_h264x_reorder_markers[pos];
		m->valid = 1U;
		m->epoch = mkv_h264x_reorder_epoch;
		m->idr_au = au;
		m->idr_ts = timestamp;
		m->zero_au = 0U;
		m->zero_ts = -1;
		m->recovery_au = 0U;
		m->recovery_ts = -1;
		m->recovery_pic_num = 0U;
	}

	mkv_h264x_reorder_decode_calls++;
	if (pic_num == 0)
		mkv_h264x_reorder_zero_calls++;
	if (pic_num > 1)
		mkv_h264x_reorder_multi_calls++;

	m = mkv_h264x_reorder_current_marker();
	since_idr = 0xffffffffU;
	if (m != 0 && au >= m->idr_au)
		since_idr = au - m->idr_au;

	if (m != 0 &&
	    m->zero_au == 0U &&
	    p->h264x.current_au.is_reference_b &&
	    pic_num == 0 &&
	    since_idr <= 8U) {
		m->zero_au = au;
		m->zero_ts = timestamp;
		m->recovery_au = 0U;
		m->recovery_ts = -1;
		m->recovery_pic_num = 0U;
		mkv_h264x_reorder_zero_refb_calls++;
	}

	if (m != 0 && m->zero_au != 0U && m->recovery_au == 0U &&
	    au > m->zero_au && pic_num > 0) {
		m->recovery_au = au;
		m->recovery_ts = timestamp;
		m->recovery_pic_num = (unsigned int)pic_num;
		mkv_h264x_reorder_recovery_calls++;
	}

	previous = mkv_h264x_reorder_last_pic_num;
	/* REORDER_ZERO/RECOVERY above already preserve the interesting zero-output
	 * boundary.  Do not mirror every routine 0<->1 transition to PSPLink. */
	
	mkv_h264x_reorder_last_pic_num = pic_num;
}

/* Called by mkv_play.c.  The target is the first display-order main non-ref B,
 * one frame before the zero-output reference-B timestamp. */
int mkv_decode_h264x_reorder_boundary_for_ts(struct mkv_decode_struct *p,
                                             int display_ts,
                                             int frame_duration,
                                             unsigned int *epoch_out,
                                             int *idr_ts_out,
                                             int *zero_ts_out,
                                             unsigned int *zero_au_out,
                                             unsigned int *recovery_au_out,
                                             unsigned int *recovery_pic_num_out)
{
	unsigned int i;
	int tolerance;

	if (mkv_h264x_reorder_owner != p)
		return 0;
	if (frame_duration <= 0)
		frame_duration = 33;
	tolerance = 3;
	for (i = 0U; i < MKV_H264X_REORDER_MARKER_COUNT; i++) {
		struct mkv_h264x_reorder_marker_struct *m;
		int target_ts;
		int delta;
		m = &mkv_h264x_reorder_markers[i];
		if (!m->valid || m->zero_au == 0U || m->zero_ts < 0)
			continue;
		target_ts = m->zero_ts - frame_duration;
		delta = display_ts - target_ts;
		if (delta < -tolerance || delta > tolerance)
			continue;
		if (epoch_out != 0)
			*epoch_out = m->epoch;
		if (idr_ts_out != 0)
			*idr_ts_out = m->idr_ts;
		if (zero_ts_out != 0)
			*zero_ts_out = m->zero_ts;
		if (zero_au_out != 0)
			*zero_au_out = m->zero_au;
		if (recovery_au_out != 0)
			*recovery_au_out = m->recovery_au;
		if (recovery_pic_num_out != 0)
			*recovery_pic_num_out = m->recovery_pic_num;
		return 1;
	}
	return 0;
}

#ifndef PPA_MKV_DIRECT_GU
#define PPA_MKV_DIRECT_GU 1
#endif

/*
 * 6.3.23 retains the 6.3.21 native-only control: Makefile.3xx may define older BFR
 * defaults on the compiler command line.  For this specific control probe we
 * force B-frame reconstruction OFF so any remaining deterministic block
 * shimmer/judder can be attributed to native B-ref / presentation / DPB timing
 * rather than to our software reconstruction output.
 */

/*
 * 6.3.23a compatibility-control lock.
 *
 * Keep every post-decode pixel mutator disabled, but preserve the established
 * pre-decode reference-management compatibility bridge.  A genuinely raw
 * Sony-only control cannot progress through the first P picture carrying the
 * B-pyramid RPLM/MMCO pattern on the current test stream: sceMpegAvcDecode
 * rejects that AU before any presentation experiment can run.
 *
 * This build therefore means "native pixels plus compatibility header bridge",
 * not "unmodified elementary stream".  Rewrite retry remains enabled as a
 * guarded fallback, and cache writeback is mandatory because rewritten AUs are
 * allocated after the frame-level cache transition.
 */
/* Production rewrite support is configured once in Makefile.3xx and shared
 * by this decoder and h264x_rewrite.c. Never override those feature macros
 * in one translation unit: doing so creates a compile/link and behavior split. */

/* Do not stack the newer MMCO-only bridge on top of the 6.3.37 bridge. */
#ifndef PPA_H264X_PRESENT_LOCKED_MODE
#define PPA_H264X_PRESENT_LOCKED_MODE 5U
#endif
#ifndef PPA_H264X_MOTION_MODE_DEFAULT
#define PPA_H264X_MOTION_MODE_DEFAULT 7U
#endif

static unsigned int mkv_rgb_source_pitch(const struct mkv_decode_struct *p)
{
	/* Sony's CSC stride follows decoded width, independently of the output
	 * scanout stride: 512 at <=480 pixels, 768 for wider AVC pictures. */
	if (p != 0 && p->reader.file.video_width > 480U)
		return 768U;
	return 512U;
}

static int PPA_MAYBE_UNUSED mkv_ge_copy_rgb_to_frame(struct mkv_decode_struct *p,
                                     unsigned int buf_id,
                                     int timestamp,
                                     unsigned int tmp,
                                     const char *tag) {
	unsigned int src_pitch = mkv_rgb_source_pitch(p);
	unsigned int dst_pitch = p->output_texture_width;
	unsigned int copy_w = p->reader.file.video_width;
	unsigned int copy_h = p->reader.file.video_height;
	int copy_result;

	unsigned int *src = (unsigned int *)ppa_gu_rgb_buffer;
	unsigned int *dst = (unsigned int *)p->video_frame_buffers[buf_id];

	if (copy_w > 480)
		copy_w = 480;

	if (copy_h > 272)
		copy_h = 272;

	copy_result = ppa_gu_copy_frame_blocking(dst, dst_pitch, src, src_pitch,
	                                        copy_w, copy_h);
	if (copy_result < 0) {
		return copy_result;
	}

	return 0;
}

static unsigned int mkv_rgb_buffer_cache_size(struct mkv_decode_struct *p) {
	unsigned int height;
	unsigned int pitch;

	if (p == 0)
		return 512U * 272U * 4U;
	pitch = mkv_rgb_source_pitch(p);
	height = (p->reader.file.video_height + 15U) & ~15U;
	if (height == 0U || height > 480U)
		height = 480U;
	return pitch * height * 4U;
}

/* Conservative Sony output remapper: policy 0 preserves native slot order;
 * policy 2 reverses only bursts whose timestamp/reference shape is recognized.
 * Allocation, CSC or metadata ambiguity falls back to the original order. */
#define PPA_H264X_OUTPUT_REMAP_DEFAULT_POLICY 2U
#define PPA_H264X_OUTPUT_SOURCE_MAIN 1U
#define PPA_H264X_OUTPUT_SOURCE_CACHED 2U
#define PPA_H264X_OUTPUT_SOURCE_REMAP_MAIN 3U
#define PPA_H264X_OUTPUT_SOURCE_REMAP_CACHED 4U
#define PPA_H264X_OUTPUT_SOURCE_BURST_MAIN 5U
#define PPA_H264X_OUTPUT_SOURCE_BURST_CACHED 6U
#define PPA_H264X_OUTPUT_SOURCE_LEDGER_MAIN 7U
#define PPA_H264X_OUTPUT_SOURCE_REMAP_ERROR 8U

struct mkv_h264x_output_remap_state {
	struct mkv_decode_struct *owner;
	struct h264x_two_picture_remap pair;
	volatile unsigned int policy;
	unsigned int main_remapped;
	unsigned int generic_pic_num;
	unsigned int generic_pending;
	int last_physical_index;
};

static struct mkv_h264x_output_remap_state mkv_h264x_output_remap;

static void mkv_h264x_output_remap_clear_pending(struct mkv_decode_struct *p)
{
	if (mkv_h264x_output_remap.owner != p)
		return;
	mkv_h264x_output_remap.pair.pending_slot0 = 0U;
	mkv_h264x_output_remap.main_remapped = 0U;
	mkv_h264x_output_remap.generic_pic_num = 0U;
	mkv_h264x_output_remap.generic_pending = 0U;
	mkv_h264x_output_remap.last_physical_index = 0;
}

static void mkv_h264x_output_remap_release(struct mkv_decode_struct *p)
{
	if (mkv_h264x_output_remap.owner != p)
		return;
	h264x_two_picture_release(&mkv_h264x_output_remap.pair);
	memset(&mkv_h264x_output_remap, 0, sizeof(mkv_h264x_output_remap));
}

void mkv_decode_h264x_output_remap_set(struct mkv_decode_struct *p,
	unsigned int policy)
{
	if (policy != 2U)
		policy = 0U;
	if (mkv_h264x_output_remap.owner != p) {
		h264x_two_picture_release(&mkv_h264x_output_remap.pair);
		memset(&mkv_h264x_output_remap, 0, sizeof(mkv_h264x_output_remap));
		mkv_h264x_output_remap.owner = p;
	}
	mkv_h264x_output_remap.policy = policy;
	/* Never discard a half-consumed two-picture burst when the input thread
	 * changes policy. The saved slot is still emitted by the next cached call;
	 * the new policy applies to the following decode burst. */
}

unsigned int mkv_decode_h264x_output_remap_get(struct mkv_decode_struct *p)
{
	if (mkv_h264x_output_remap.owner != p)
		return 0U;
	return mkv_h264x_output_remap.policy;
}

static int mkv_h264x_output_remap_strict_shape(struct mkv_decode_struct *p)
{
	const struct mkv_h264x_present_risk_struct *a, *b;
	struct h264x_picture_identity ia, ib;
	if (p == 0 || p->h264x_present_queue_size < 2U) return 0;
	a = &p->h264x_present_queue[0]; b = &p->h264x_present_queue[1];
	ia.valid = a->valid; ia.timestamp = a->timestamp; ia.poc_lsb = a->poc_lsb;
	ia.is_b = a->is_b; ia.is_reference_b = a->is_reference_b;
	ib.valid = b->valid; ib.timestamp = b->timestamp; ib.poc_lsb = b->poc_lsb;
	ib.is_b = b->is_b; ib.is_reference_b = b->is_reference_b;
	return h264x_two_picture_shape(&ia, &ib, p->video_frame_duration);
}

static int mkv_h264x_output_remap_generic_shape(
	struct mkv_decode_struct *p, unsigned int pic_num)
{
	unsigned int i;
	unsigned int poc_bits;
	unsigned int poc_modulus;
	int frame_duration;

	if (p == 0 || pic_num < 2U ||
	    p->h264x_present_queue_size < pic_num)
		return 0;
	frame_duration = p->video_frame_duration;
	if (frame_duration <= 0)
		frame_duration = 33;
	poc_bits = p->h264x.sps.log2_max_pic_order_cnt_lsb_minus4 + 4U;
	if (poc_bits < 4U || poc_bits > 16U)
		return 0;
	poc_modulus = 1U << poc_bits;
	for (i = 0U; i < pic_num; i++) {
		const struct mkv_h264x_present_risk_struct *cur;
		cur = &p->h264x_present_queue[i];
		if (!cur->valid)
			return 0;
		if (i != 0U) {
			const struct mkv_h264x_present_risk_struct *prev;
			int ts_delta;
			unsigned int poc_delta;
			prev = &p->h264x_present_queue[i - 1U];
			ts_delta = cur->timestamp - prev->timestamp;
			if (ts_delta < frame_duration - 2 ||
			    ts_delta > frame_duration + 2)
				return 0;
			{
				int cur_full;
				int prev_full;
				unsigned int cur_epoch;
				unsigned int prev_epoch;
				int have_cur;
				int have_prev;
				have_cur = mkv_h264x_poc_identity_lookup(
					cur->timestamp, cur->au_index, &cur_full, &cur_epoch);
				have_prev = mkv_h264x_poc_identity_lookup(
					prev->timestamp, prev->au_index, &prev_full, &prev_epoch);
				if (have_cur && have_prev && cur_epoch != 0U &&
				    cur_epoch == prev_epoch) {
					if (cur_full - prev_full != 2)
						return 0;
				}
				else {
					poc_delta = (cur->poc_lsb + poc_modulus -
					             prev->poc_lsb) & (poc_modulus - 1U);
					if (poc_delta != 2U)
						return 0;
				}
			}
		}
	}
	return 1;
}

static unsigned int mkv_h264x_output_remap_prepare_main(
	struct mkv_decode_struct *p, int pic_num)
{
	unsigned int bytes;
	unsigned int policy;
	unsigned int requested_policy;
	int strict;

	if (p == 0 || p->video_format != 0x61766331 || pic_num < 2)
		return PPA_H264X_OUTPUT_SOURCE_MAIN;
	if (mkv_h264x_output_remap.owner != p)
		mkv_decode_h264x_output_remap_set(p,
			PPA_H264X_OUTPUT_REMAP_DEFAULT_POLICY);
	requested_policy = mkv_h264x_output_remap.policy;
	policy = requested_policy;

	/* 6.3.39: Sony detail2 bursts are exposed in reverse physical order.
	 * Generalize the proven 2-slot mapping to any burst length without extra
	 * frame surfaces: main uses physical K-1, then cached calls walk K-2..0. */
	/* Preserve the proven 6.3.37 two-picture remap for every stream, including
	 * CABAC compatibility contexts.  The generalized walker is needed only
	 * when Sony actually returns three or more pictures in one decode call. */
	if (p->h264x_compat_generic_burst_reverse && pic_num > 2) {
		char *generic_result;
		int generic_strict;
		generic_strict = mkv_h264x_output_remap_generic_shape(
			p, (unsigned int)pic_num);
		if (policy == 0U) {
			return PPA_H264X_OUTPUT_SOURCE_MAIN;
		}
		
		if (!generic_strict) {
			return PPA_H264X_OUTPUT_SOURCE_MAIN;
		}
		if ((unsigned int)pic_num > p->h264x_compat_pic_max)
			p->h264x_compat_pic_max = (unsigned int)pic_num;
		if (mkv_h264x_output_remap.generic_pending != 0U) {
			mkv_h264x_output_remap_clear_pending(p);
			return PPA_H264X_OUTPUT_SOURCE_MAIN;
		}
		generic_result = mp4_avc_get_cache(&p->avc, ppa_gu_rgb_buffer, 1);
		
		if (generic_result != 0) {
			return PPA_H264X_OUTPUT_SOURCE_MAIN;
		}
		mkv_h264x_output_remap.generic_pic_num = (unsigned int)pic_num;
		mkv_h264x_output_remap.generic_pending = (unsigned int)pic_num - 1U;
		mkv_h264x_output_remap.last_physical_index = pic_num - 1;
		return PPA_H264X_OUTPUT_SOURCE_BURST_MAIN;
	}
	if (pic_num != 2)
		return PPA_H264X_OUTPUT_SOURCE_MAIN;
	if (policy == 0U) {
		return PPA_H264X_OUTPUT_SOURCE_MAIN;
	}
	strict = mkv_h264x_output_remap_strict_shape(p);
	
	if (!strict) {
		return PPA_H264X_OUTPUT_SOURCE_MAIN;
	}
	
	bytes = mkv_rgb_buffer_cache_size(p);
	{
		unsigned int pitch = mkv_rgb_source_pitch(p);
		int remapped = h264x_two_picture_prepare(&mkv_h264x_output_remap.pair,
			&p->avc, ppa_gu_rgb_buffer, pitch, bytes / (pitch * 4U));
		if (remapped < 0) return PPA_H264X_OUTPUT_SOURCE_REMAP_ERROR;
		if (!remapped) return PPA_H264X_OUTPUT_SOURCE_MAIN;
	}
	mkv_h264x_output_remap.main_remapped = 1U;
	mkv_h264x_output_remap.last_physical_index = 1;

	return PPA_H264X_OUTPUT_SOURCE_REMAP_MAIN;
}

static int mkv_h264x_output_remap_restore_cached(
	struct mkv_decode_struct *p, unsigned int pic_num,
	unsigned int *source_kind_out)
{
	unsigned int bytes;
	if (source_kind_out != 0)
		*source_kind_out = PPA_H264X_OUTPUT_SOURCE_CACHED;
	if (p == 0 || mkv_h264x_output_remap.owner != p)
		return 0;
	if (mkv_h264x_output_remap.generic_pending != 0U) {
		unsigned int k;
		unsigned int cache_arg;
		char *generic_result;
		k = mkv_h264x_output_remap.generic_pic_num;
		if (k < 2U || pic_num == 0U || pic_num >= k) {
			mkv_h264x_output_remap_clear_pending(p);
			return 0;
		}
		cache_arg = k - pic_num + 1U;
		mkv_h264x_output_remap.last_physical_index = (int)pic_num - 1;
		generic_result = mp4_avc_get_cache(&p->avc, ppa_gu_rgb_buffer,
		                                   (int)cache_arg);
		if (generic_result != 0) {
			mkv_h264x_output_remap_clear_pending(p);
			return 0;
		}
		mkv_h264x_output_remap.generic_pending--;
		if (mkv_h264x_output_remap.generic_pending == 0U)
			mkv_h264x_output_remap.generic_pic_num = 0U;
		if (source_kind_out != 0)
			*source_kind_out = PPA_H264X_OUTPUT_SOURCE_BURST_CACHED;
		return 1;
	}
	bytes = mkv_rgb_buffer_cache_size(p);
	{
		unsigned int pitch = mkv_rgb_source_pitch(p);
		int restored = h264x_two_picture_restore(&mkv_h264x_output_remap.pair,
			ppa_gu_rgb_buffer, pitch, bytes / (pitch * 4U), pic_num);
		if (restored <= 0) return restored;
	}

	mkv_h264x_output_remap.main_remapped = 0U;
	mkv_h264x_output_remap.last_physical_index = 0;
	if (source_kind_out != 0)
		*source_kind_out = PPA_H264X_OUTPUT_SOURCE_REMAP_CACHED;
	return 1;
}

/* Output identity belongs to playback: reference restoration consumes these
 * fields even when no developer capture or overlay exists. */
static void mkv_h264x_clear_output_identity(struct mkv_decode_struct *p)
{
    unsigned int i;
    for (i = 0; i < mkv_maximum_frame_buffers; ++i) {
        struct mkv_decode_buffer_struct *b = &p->output_video_frame_buffers[i];
        b->h264x_valid = 0U;
        b->h264x_source_kind = 0U;
        b->h264x_native_output_seq = 0U;
        b->h264x_au_index = 0U;
        b->h264x_poc_lsb = 0U;
        b->h264x_is_b = 0U;
        b->h264x_is_reference_b = 0U;
        b->h264x_is_p = 0U;
        b->h264x_risk = 0U;
        b->h264x_risk_class = 0U;
        b->h264x_rplm_found = 0U;
        b->h264x_mmco_found = 0U;
    }
}

static void mkv_h264x_stamp_output_buffer(
    struct mkv_decode_struct *p, unsigned int source_kind,
    const struct mkv_h264x_present_risk_struct *meta)
{
    struct mkv_decode_buffer_struct *b;
    if (p->current_video_buffer_number >= p->number_of_frame_buffers)
        return;
    b = &p->output_video_frame_buffers[p->current_video_buffer_number];
    b->h264x_valid = 1U;
    b->h264x_source_kind = source_kind;
    if (++p->h264x_output_sequence == 0U)
        ++p->h264x_output_sequence;
    b->h264x_native_output_seq = p->h264x_output_sequence;
    b->h264x_au_index = meta->au_index;
    b->h264x_poc_lsb = meta->poc_lsb;
    b->h264x_is_b = meta->is_b != 0;
    b->h264x_is_reference_b = meta->is_reference_b != 0;
    b->h264x_is_p = meta->is_p != 0;
    b->h264x_risk = meta->risk;
    b->h264x_risk_class = meta->risk_class;
    b->h264x_rplm_found = meta->rplm_found;
    b->h264x_mmco_found = meta->mmco_found;
}

static void mkv_present_rgb_frame(struct mkv_decode_struct *p,
                                  unsigned int current_buffer,
                                  unsigned int aspect_ratio,
                                  unsigned int zoom,
                                  unsigned int luminosity_boost,
                                  unsigned int show_interface,
                                  unsigned int show_subtitle,
                                  unsigned int subtitle_format,
                                  unsigned int frame_number,
                                  int timestamp,
                                  const char *tag) {
#if PPA_MKV_DIRECT_GU

	{
		int direct_scanout = ppa_gu_direct_scanout_enabled();
		int pipeline_active = !direct_scanout && ppa_video_pipeline_enabled();
		void *frame = p->video_frame_buffers[current_buffer];

		if (pipeline_active) {
			ppa_gu_draw_video_only(aspect_ratio, zoom, luminosity_boost,
			                       subtitle_format, frame_number, frame);
		}
		else {
			ppa_gu_draw(aspect_ratio, zoom, luminosity_boost,
			            show_interface, show_subtitle, subtitle_format,
			            frame_number, frame);
		}

		p->output_video_frame_buffers[current_buffer].data = frame;
		ppa_gu_wait();

		/* GE-rendered output proceeds directly to the display.  Do not touch
		 * every output pixel from the CPU merely to hand ownership between two
		 * devices that already observe main RAM/eDRAM coherently. */
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

#else
	if (mkv_ge_copy_rgb_to_frame(p,
	                             current_buffer,
	                             timestamp,
	                             frame_number,
	                             tag) < 0) {
		return;
	}
	ppa_video_pipeline_process(p->video_frame_buffers[current_buffer],
	                           (unsigned int)p->output_texture_width,
	                           p->reader.file.video_width,
	                           p->reader.file.video_height,
	                           aspect_ratio, zoom, frame_number);

	ppa_gu_overlay_cpu_frame(show_interface,
	                         show_subtitle,
	                         subtitle_format,
	                         frame_number,
	                         p->video_frame_buffers[current_buffer],
	                         p->output_texture_width);
	ppa_cache_wb_range(p->video_frame_buffers[current_buffer],
	                   (unsigned int)p->output_texture_width * 272U * 4U);

	p->output_video_frame_buffers[current_buffer].data =
		p->video_frame_buffers[current_buffer];
#endif
}

static int in_mkv_timestamp_queue(int* queue,
                                  unsigned int* queue_size,
                                  unsigned int queue_max,
                                  int timestamp) {
	if (queue == 0 || queue_size == 0)
		return 0;
	if (*queue_size + 1 > queue_max) {
		return 0;
	}

	unsigned int i;

	for (i = 0U; i < *queue_size; i++) {
		if (timestamp < queue[i])
			break;
	}
	if (i < *queue_size) {
		memmove(&queue[i + 1U], &queue[i],
		        (*queue_size - i) * sizeof(queue[0]));
	}

	queue[i] = timestamp;
	*queue_size += 1;

	return 1;
}

static int out_mkv_timestamp_queue(int* queue,
                                   unsigned int* queue_size,
                                   unsigned int queue_max,
                                   int* timestamp) {
	if (timestamp != 0)
		*timestamp = -1;
	if (queue == 0 || queue_size == 0 || timestamp == 0 || *queue_size == 0)
		return 0;
	if (*queue_size > queue_max) {
		*queue_size = 0U;
		return 0;
	}

	*timestamp = queue[0];

	unsigned int i;

	for (i = 1U; i < *queue_size; i++)
		queue[i - 1] = queue[i];

	queue[i - 1] = -1;
	*queue_size -= 1;

	return 1;
}

static void clear_mkv_timestamp_queue(int* queue,
                                      unsigned int* queue_size,
                                      unsigned int queue_max) {
	unsigned int i;

	for (i = 0U; i < queue_max; i++) {
		queue[i] = -1;
	}

	*queue_size = 0;

}

static void clear_mkv_h264x_present_queue(struct mkv_h264x_present_risk_struct *queue,
                                          unsigned int *queue_size,
                                          unsigned int queue_max) {
	unsigned int i;

	if (queue == 0 || queue_size == 0)
		return;

	for (i = 0; i < queue_max; i++) {
		memset(&queue[i], 0, sizeof(queue[i]));
		queue[i].timestamp = -1;
	}

	*queue_size = 0;
}

static int in_mkv_h264x_present_queue(struct mkv_h264x_present_risk_struct *queue,
                                      unsigned int *queue_size,
                                      unsigned int queue_max,
                                      const struct mkv_h264x_present_risk_struct *meta) {
	unsigned int i;
	int j;

	if (queue == 0 || queue_size == 0 || meta == 0)
		return 0;
	if (*queue_size + 1 > queue_max)
		return 0;

	for (i = 0; i < *queue_size; i++) {
		if (meta->timestamp < queue[i].timestamp) {
			for (j = (int)*queue_size - 1; j >= (int)i; j--) {
				queue[j + 1] = queue[j];
			}
			break;
		}
	}

	queue[i] = *meta;
	*queue_size += 1;

	return 1;
}

static int out_mkv_h264x_present_queue(struct mkv_h264x_present_risk_struct *queue,
                                       unsigned int *queue_size,
                                       unsigned int queue_max,
                                       struct mkv_h264x_present_risk_struct *meta) {
	unsigned int i;

	if (meta != 0) {
		memset(meta, 0, sizeof(*meta));
		meta->timestamp = -1;
	}

	if (queue == 0 || queue_size == 0 || *queue_size == 0)
		return 0;

	if (meta != 0)
		*meta = queue[0];

	for (i = 1; i < *queue_size; i++)
		queue[i - 1] = queue[i];

	memset(&queue[i - 1], 0, sizeof(queue[i - 1]));
	queue[i - 1].timestamp = -1;
	*queue_size -= 1;

	(void)queue_max;
	return 1;
}

static int mkv_h264x_pop_presentation_pair(
    struct mkv_decode_struct *p,
    const char *where,
    int *timestamp,
    struct mkv_h264x_present_risk_struct *meta)
{
	int ts_ok;
	int meta_ok;
	unsigned int ts_before;
	unsigned int meta_before;

	if (timestamp != 0)
		*timestamp = -1;
	if (meta != 0)
		memset(meta, 0, sizeof(*meta));
	if (p == 0 || timestamp == 0 || meta == 0)
		return 0;

	ts_before = p->timestamp_queue_size;
	meta_before = p->h264x_present_queue_size;
	ts_ok = out_mkv_timestamp_queue(p->timestamp_queue,
	                                &p->timestamp_queue_size,
	                                mkv_timestamp_queue_max,
	                                timestamp);
	meta_ok = out_mkv_h264x_present_queue(p->h264x_present_queue,
	                                      &p->h264x_present_queue_size,
	                                      mkv_timestamp_queue_max,
	                                      meta);
	if (!ts_ok || !meta_ok || ts_before != meta_before ||
	    (meta->timestamp != -1 && meta->timestamp != *timestamp)) {
		return 0;
	}
	return 1;
}

static int mkv_h264x_present_queue_add_empty(struct mkv_decode_struct *p, int timestamp) {
	struct mkv_h264x_present_risk_struct meta;
	int ok;

	if (p == 0)
		return 0;

	memset(&meta, 0, sizeof(meta));
	meta.timestamp = timestamp;

	ok = in_mkv_h264x_present_queue(p->h264x_present_queue,
	                                &p->h264x_present_queue_size,
	                                mkv_timestamp_queue_max,
	                                &meta);
	
	return ok;
}

static int mkv_h264x_present_queue_add_probe(struct mkv_decode_struct *p,
                                             const struct h264x_probe *probe,
                                             int timestamp) {
	struct mkv_h264x_present_risk_struct meta;
	const struct h264x_au_info *au;
	const struct h264x_dpb *dpb;

	if (p == 0 || probe == 0)
		return 0;

	au = &probe->current_au;
	dpb = &probe->shadow_dpb;

	memset(&meta, 0, sizeof(meta));
	meta.valid = 1;
	meta.timestamp = timestamp;
	meta.au_index = au->au_index;
	meta.au_size = au->au_size;
	meta.frame_num = au->frame_num;
	meta.poc_lsb = au->pic_order_cnt_lsb;
	meta.slice_type = au->slice_type;
	meta.nal_ref_idc = au->nal_ref_idc;
	meta.is_i = au->is_i;
	meta.is_p = au->is_p;
	meta.is_b = au->is_b;
	meta.is_reference_b = au->is_reference_b;
	meta.risk = dpb->last_visible_risk;
	meta.risk_class = dpb->last_risk_class;
	meta.rplm_found = dpb->last_rplm_found;
	meta.rplm_missing = dpb->last_rplm_missing;
	meta.mmco_found = dpb->last_mmco_found;
	meta.mmco_absent = dpb->last_mmco_absent;
	meta.true_missing_total = dpb->true_missing_ref_events;
	meta.absent_total = dpb->mmco_absent_events;
	meta.benign_absent_total = dpb->mmco_benign_absent_events;
	mkv_h264x_poc_identity_store(p, au, timestamp);

	{
		int ok = in_mkv_h264x_present_queue(p->h264x_present_queue,
		                                    &p->h264x_present_queue_size,
		                                    mkv_timestamp_queue_max,
		                                    &meta);
		
		return ok;
	}
}

void mkv_decode_safe_constructor(struct mkv_decode_struct *p) {
	p->audio_decoder = -1;
	p->audio_stream = 0;
	p->audio_stream_selected = 0;
	p->h264x_output_sequence = 0U;
	mkv_h264x_reorder_reset(p);
	mkv_read_safe_constructor(&p->reader);

	mp4_avc_safe_constructor(&p->avc);
	h264x_probe_init(&p->h264x);
	/* These two control words are deliberately present in every struct layout.
	 * Keep them deterministic even when their optional implementations are out. */

	memset(p->output_video_frame_buffers, 0, sizeof(p->output_video_frame_buffers));
	memset(p->output_audio_frame_buffers, 0, sizeof(p->output_audio_frame_buffers));
	int i = 0;
	for (; i < mkv_maximum_frame_buffers; i++) {
		p->video_frame_buffers[i] = 0;
		p->audio_frame_buffers[i] = 0;
	}
	
	clear_mkv_timestamp_queue(p->timestamp_queue, &p->timestamp_queue_size, mkv_timestamp_queue_max);
	clear_mkv_h264x_present_queue(p->h264x_present_queue, &p->h264x_present_queue_size, mkv_timestamp_queue_max);
	p->avc_first_packet_done = 0;
	p->h264x_compat_active = 0U;
	p->h264x_compat_flags = 0U;
	p->h264x_compat_original_refs = 0U;
	p->h264x_compat_effective_refs = 0U;
	p->h264x_compat_force_level_idc = 0U;
	p->h264x_compat_mpeg_mode = 4U;
	p->h264x_compat_use_original_ps = 0U;
	p->h264x_compat_disable_weights = 0U;
	p->h264x_compat_flatten_brefs = 0U;
	p->h264x_compat_retry_stage = 0U;
	p->h264x_compat_generic_burst_reverse = 0U;
	p->h264x_compat_first_idr_retry_attempted = 0U;
	p->h264x_compat_pic_max = 0U;
	p->last_audio_timestamp = 0;
	p->last_video_timestamp = 0;
	p->is_eof = 0;
	p->output_epoch = 1U;

}

void mkv_decode_close(struct mkv_decode_struct *p, int pspType) {
	(void)pspType;

	/* Release the software remap on both failed-open and normal close paths.
	 * Normal play_close has already retired firmware before card/stat I/O;
	 * no pending remap here is an independently running ME/VME job. */
	mkv_h264x_output_remap_release(p);

    ppu_audio_stream_close(p->audio_stream);
    p->audio_stream = 0;
	media_codecs_close(&p->avc, &p->audio_decoder);
	mkv_read_close(&p->reader);

	clear_reset_framebuffer();
	h264x_probe_close(&p->h264x);

	int i = 0;
	for (; i < mkv_maximum_frame_buffers; i++) {
		if (p->audio_frame_buffers[i] != 0)
			free_64(p->audio_frame_buffers[i]);
	}

	mkv_decode_safe_constructor(p);
}

static void mkv_h264x_compat_select(struct mkv_decode_struct *p)
{
	unsigned int flags;

	if (p == 0)
		return;
	p->h264x_compat_active = 0U;
	p->h264x_compat_flags = 0U;
	p->h264x_compat_original_refs = 0U;
	p->h264x_compat_effective_refs = 0U;
	p->h264x_compat_force_level_idc = 0U;
	p->h264x_compat_mpeg_mode = 4U;
	p->h264x_compat_use_original_ps = 0U;
	p->h264x_compat_disable_weights = 0U;
	p->h264x_compat_flatten_brefs = 0U;
	p->h264x_compat_retry_stage = 0U;
	p->h264x_compat_generic_burst_reverse = 0U;
	if (!p->h264x.sps.valid || !p->h264x.pps.valid)
		return;

	{
		struct h264x_compat_selection selection;
		h264x_compat_select(&p->h264x, &selection);
		flags = selection.flags;
		p->h264x_compat_flags = flags;
		p->h264x_compat_original_refs = selection.original_refs;
		p->h264x_compat_effective_refs = selection.effective_refs;
		p->h264x_compat_disable_weights = selection.disable_weights;
	}
	p->h264x_compat_flatten_brefs = 0U;
	p->h264x_compat_generic_burst_reverse = flags ? 1U : 0U;
	p->h264x_compat_active = flags ? 1U : 0U;
	if (p->h264x_compat_active) {
		p->h264x_compat_mpeg_mode = 5U;
		p->h264x_compat_use_original_ps =
			p->h264x_compat_disable_weights ? 0U : 1U;
	}

}

static int mkv_h264x_boot_me_for_avc_context(
	struct mkv_decode_struct *p, int mpeg_mode, int avc_profile,
	const char *stage)
{
	int boot_type;
	int boot_result;

	if (mpeg_mode == 5)
		boot_type = 1;
	else if (avc_profile == 0x42)
		boot_type = 4;
	else
		boot_type = 3;
	boot_result = me_boot_start(boot_type);
	
	return boot_result;
}

static char *mkv_h264x_open_avc_context_compat(struct mkv_decode_struct *p,
                                               unsigned int force_level_idc,
                                               unsigned int force_refs)
{
	mkvinfo_track_t *video_track;
	int avc_profile;
	int avc_nal_prefix_size;
	int avc_sps_size;
	int avc_pps_size;
	int mpeg_mode;
	void *sps_data;
	void *pps_data;
	void *compat_sps;
	void *compat_pps;
	unsigned int compat_sps_size;
	unsigned int compat_pps_size;
	unsigned int target_refs;
	char *result;

	if (p == 0 || p->reader.file.info == 0)
		return "h264x compat open: invalid decode context";
	video_track = p->reader.file.info->tracks[p->reader.file.video_track_id];
	if (video_track == 0 || video_track->private_data == 0 ||
	    video_track->private_size < 16U)
		return "h264x compat open: invalid avcC";

	avc_profile = video_track->private_data[1];
	avc_nal_prefix_size = (video_track->private_data[4] & 0x03) + 1;
	avc_sps_size = video_track->private_data[6];
	avc_sps_size = (avc_sps_size << 8) | video_track->private_data[7];
	if ((unsigned int)(11 + avc_sps_size) >= video_track->private_size)
		return "h264x compat open: SPS overrun";
	avc_pps_size = video_track->private_data[9 + avc_sps_size];
	avc_pps_size = (avc_pps_size << 8) |
		video_track->private_data[10 + avc_sps_size];
	if ((unsigned int)(11 + avc_sps_size + avc_pps_size) >
	    video_track->private_size)
		return "h264x compat open: PPS overrun";

	sps_data = video_track->private_data + 8;
	pps_data = video_track->private_data + 8 + avc_sps_size + 3;
	ppa_video_pipeline_set_avc_sps((const unsigned char *)sps_data,
	                               (unsigned int)avc_sps_size);
	mpeg_mode = ((p->reader.file.video_width > 480 ||
	              p->reader.file.video_height > 272) ? 5 : 4);
	if (p->h264x_compat_active &&
	    (p->h264x_compat_mpeg_mode == 4U ||
	     p->h264x_compat_mpeg_mode == 5U))
		mpeg_mode = (int)p->h264x_compat_mpeg_mode;

	if (!p->h264x_compat_active || p->h264x_compat_use_original_ps) {
		
		if (mkv_h264x_boot_me_for_avc_context(p, mpeg_mode, avc_profile,
		                                    "original_ps") != 0)
			return "mkv h264x: Sony ME boot failed (see boot log)";
		return mp4_avc_open(&p->avc, avc_profile, mpeg_mode,
		                    sps_data, avc_sps_size,
		                    pps_data, avc_pps_size,
		                    avc_nal_prefix_size);
	}

	target_refs = force_refs != 0U ? force_refs :
		p->h264x_compat_effective_refs;
	if (target_refs == 0U)
		target_refs = 1U;
	compat_sps = malloc_64((unsigned int)avc_sps_size + 64U);
	compat_pps = malloc_64((unsigned int)avc_pps_size + 64U);
	if (compat_sps == 0 || compat_pps == 0) {
		if (compat_sps) free_64(compat_sps);
		if (compat_pps) free_64(compat_pps);
		return "h264x compat open: parameter-set allocation failed";
	}
	compat_sps_size = 0U;
	compat_pps_size = 0U;
	if (!h264x_rewrite_parameter_sets_compat(
	        sps_data, (unsigned int)avc_sps_size,
	        pps_data, (unsigned int)avc_pps_size,
	        target_refs, force_level_idc,
	        p->h264x_compat_disable_weights,
	        compat_sps, (unsigned int)avc_sps_size + 64U,
	        &compat_sps_size,
	        compat_pps, (unsigned int)avc_pps_size + 64U,
	        &compat_pps_size)) {
		free_64(compat_sps);
		free_64(compat_pps);
		return "h264x compat open: parameter-set rewrite failed";
	}

	p->h264x_compat_effective_refs = target_refs;
	p->h264x_compat_force_level_idc = force_level_idc;

	if (mkv_h264x_boot_me_for_avc_context(p, mpeg_mode, avc_profile,
	                                    "normalized_ps") != 0) {
		free_64(compat_sps);
		free_64(compat_pps);
		return "mkv h264x: Sony ME boot failed (see boot log)";
	}
	result = mp4_avc_open(&p->avc, avc_profile, mpeg_mode,
	                      compat_sps, (int)compat_sps_size,
	                      compat_pps, (int)compat_pps_size,
	                      avc_nal_prefix_size);
	free_64(compat_sps);
	free_64(compat_pps);
	return result;
}

static char *mkv_h264x_retry_first_idr_stage(
	struct mkv_decode_struct *p,
	void *decode_buffer, unsigned int decode_size,
	unsigned int force_level_idc, unsigned int force_refs,
	const char *stage_name, int *pic_num_out)
{
	char *open_result;
	char *decode_result;
	int retry_pic_num;

	if (p == 0 || decode_buffer == 0 || decode_size == 0U)
		return "h264x compat retry: invalid arguments";
	mp4_avc_close(&p->avc);
	open_result = mkv_h264x_open_avc_context_compat(
		p, force_level_idc, force_refs);
	if (open_result != 0 && p->h264x_compat_mpeg_mode == 5U) {
		p->h264x_compat_mpeg_mode = 4U;
		open_result = mkv_h264x_open_avc_context_compat(
			p, force_level_idc, force_refs);
	}
	if (open_result != 0) {
		return open_result;
	}
	retry_pic_num = 0;
	ppa_cache_cpu_wrote_me_will_read(decode_buffer, decode_size);
	decode_result = mp4_avc_get(&p->avc, 3, decode_buffer, decode_size,
	                            ppa_gu_rgb_buffer, &retry_pic_num);
	if (pic_num_out != 0)
		*pic_num_out = retry_pic_num;
	return decode_result;
}

static char *mkv_decode_reopen_avc_context(struct mkv_decode_struct *p) {
	if (p == 0)
		return "mkv_decode_reopen_avc_context: null decode";
	if (p->video_format != 0x61766331)
		return "mkv_decode_reopen_avc_context: not avc";
	mkv_h264x_output_remap_clear_pending(p);
	mp4_avc_close(&p->avc);
	return mkv_h264x_open_avc_context_compat(
		p, p->h264x_compat_force_level_idc, 0U);
}

char *mkv_decode_open(struct mkv_decode_struct *p, char *s, int pspType, int tvAspectRatio, int tvWidth, int tvHeight, int videoMode) {
	
	mkv_decode_safe_constructor(p);
	
	cpu_clock_set_maximum();
	char *result = mkv_read_open(&p->reader, s);
	if (result != 0) {
		mkv_decode_close(p, pspType);
		return(result);
	}

	p->audio_frame_size = (p->reader.file.audio_resample_scale << 1) << p->reader.file.audio_stereo;

	p->video_format = p->reader.file.video_type;
	p->avc_first_packet_done = 0;

	if (!ppa_session_audio_only()) {
	if (p->video_format == 0x61766331 /*avc1*/ ) {
		mkvinfo_track_t *video_track = p->reader.file.info->tracks[p->reader.file.video_track_id];
		h264x_probe_open_avcc(&p->h264x,
		                     video_track->private_data,
		                     video_track->private_size,
		                     p->reader.file.video_width,
		                     p->reader.file.video_height);
		mkv_read_set_video_reorder_requirement(&p->reader,
		                                       p->h264x.sps.num_ref_frames);
		if (!ppa_video_pipeline_extreme_battery_saver_enabled())
			mkv_h264x_compat_select(p);
	}
	
	/* AVC ME boot mode is selected together with the Sony MPEG context in
	 * mkv_h264x_open_avc_context_compat().  This is important for the
	 * expanded mode-5 compatibility attempt: MPEG mode 5 must use ME boot 1,
	 * while mode-4 Main/Baseline contexts use boot 3/4 respectively. */
	result = mkv_h264x_open_avc_context_compat(
		p, p->h264x_compat_force_level_idc,
		p->h264x_compat_effective_refs);
	if (result != 0 && p->h264x_compat_active &&
	    p->h264x_compat_mpeg_mode == 5U) {
		/* Apply the same mode-4 open fallback to the initial weights-off
		 * context; changing the initial PPS must not lose this recovery rung. */
		p->h264x_compat_use_original_ps = 0U;
		p->h264x_compat_mpeg_mode = 4U;
		result = mkv_h264x_open_avc_context_compat(
			p, p->h264x_compat_force_level_idc,
			p->h264x_compat_effective_refs);
	}
	if (result != 0) {
		mkv_decode_close(p, pspType);
		return(result);
	}

	{
		unsigned int display_width = p->reader.file.display_width;
		unsigned int display_height = p->reader.file.display_height;

		/*
		* MKV display_width/display_height can be absent, zero, or parser-derived
		* in a way that produces unusable aspect geometry. the MP4 path already falls
		* back to coded video dimensions before aspect_ratio_struct_init().
		*/
		if (display_width == 0 || display_height == 0) {
			display_width = p->reader.file.video_width;
			display_height = p->reader.file.video_height;
		}

		if (display_width > 4096 || display_height > 4096) {
			display_width = p->reader.file.video_width;
			display_height = p->reader.file.video_height;
		}

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
	
	}

	//*/
    p->audio_stream = ppu_audio_stream_open(&p->reader.file.audio_formats[0]);
    if (!p->audio_stream) {
        mkv_decode_close(p, pspType);
        return "audio: decoder or resampler initialization failed";
    }
    p->audio_decoder = audio_decoder_is_open() ? 0 : -1;

	cpu_clock_set_minimum();

	unsigned int i = 0U;
	if (ppa_session_audio_only()) {
			p->number_of_frame_buffers = 4; /* audio ring, no video producer */
			p->output_texture_width = ppa_memory_fixed_region_reserved() ? 768 : 512;
			p->video_frame_size = 0;
			p->video_frame_buffers[0] = ppa_memory_fixed_region_reserved() ?
			    ppa_memory_extended_frame_surface(0) : psp1k_get_frame_buffer(0);
			if (!p->video_frame_buffers[0]) {
				mkv_decode_close(p, pspType);
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
				mkv_decode_close(p, pspType);
				return("mkv_decode_open: invalid direct scanout surface");
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
				mkv_decode_close(p, pspType);
				return("mkv_decode_open: invalid extended frame surface");
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
	
	i = 0;
	for (; i < p->number_of_frame_buffers; i++) {
		/* Output-only ring: decode/SRC scratch belongs to audio_stream. */
	
		p->audio_frame_buffers[i] = malloc_64(p->audio_frame_size);

		if (p->audio_frame_buffers[i] == 0) {
			mkv_decode_close(p, pspType);
			return("mkv_decode_open: malloc_64 failed on audio_frame_buffers");
		}

		memset(p->audio_frame_buffers[i], 0, p->audio_frame_size);
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
				mkv_decode_close(p, pspType);
				return "mkv_decode_open: TV surface clear failed";
			}
		}
	}

	sceDisplayWaitVblankStart();
	if (!ppa_session_audio_only() && ppa_gu_direct_scanout_enabled()) {
		void *bootstrap = ppa_memory_extended_frame_surface(0);
		if (bootstrap == 0) {
			mkv_decode_close(p, pspType);
			return("mkv_decode_open: missing direct-scanout bootstrap surface");
		}
		if (ppa_gu_clear_frame_blocking(bootstrap, 768U, 480U, 272U) < 0) {
			mkv_decode_close(p, pspType);
			return("mkv_decode_open: GE bootstrap clear failed");
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
			mkv_decode_close(p, pspType);
			return("mkv_decode_open: GE initial framebuffer clear failed");
		}
		sceDisplaySetFrameBuf(p->video_frame_buffers[0],
		                      p->output_texture_width,
		                      PSP_DISPLAY_PIXEL_FORMAT_8888,
		                      PSP_DISPLAY_SETBUF_IMMEDIATE);
	}

	p->current_video_buffer_number = 0;
	p->current_audio_buffer_number = 0;
	
	uint64_t duration = 1000LL;
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

int   mkv_decode_is_eof(struct mkv_decode_struct *p) {
	return (p->is_eof);
}

void mkv_decode_reset(struct mkv_decode_struct *p) {
	char *result = mkv_decode_seek(p, 0, 0);
	if (result != 0) {
		p->is_eof = 1;
	}
}

char *mkv_decode_seek(struct mkv_decode_struct *p, int timestamp, int last_timestamp) {
	char *result;

	if (p == 0)
		return "mkv_decode_seek: null decoder";
	result = mkv_read_seek(&p->reader, timestamp, last_timestamp);
	if (result != 0)
		return result;

    if (!ppu_audio_stream_reset(p->audio_stream, timestamp <= 0))
        return "audio: seek reset failed";

	/* A successful container seek invalidates both the software reorder sidecar
	 * and Sony's hidden AVC/CABAC state. Reopen before admitting the target IDR. */
	result = mkv_decode_reopen_avc_context(p);
	if (result != 0)
		return result;
	mkv_h264x_output_remap_clear_pending(p);
	mkv_h264x_reorder_reset(p);
	mkv_h264x_clear_output_identity(p);
	p->avc_first_packet_done = 0;
	p->is_eof = 0;
	p->output_epoch++;
	if (p->output_epoch == 0U)
		p->output_epoch = 1U;
	clear_mkv_timestamp_queue(p->timestamp_queue, &p->timestamp_queue_size, mkv_timestamp_queue_max);
	clear_mkv_h264x_present_queue(p->h264x_present_queue, &p->h264x_present_queue_size, mkv_timestamp_queue_max);
	h264x_probe_reset_stream(&p->h264x);
	p->last_audio_timestamp = timestamp;
	p->last_video_timestamp = timestamp;
	return 0;
}

static char *mkv_audio_packet(void *reader, unsigned int track,
                                  struct ppa_media_packet *packet)
{
    char *error = mkv_read_get_audio((struct mkv_read_struct *)reader, track, packet);
    if (error && !strcmp(error, MKV_READ_EOF)) return PPU_AUDIO_EOF;
    if (error && !strcmp(error, MKV_READ_AUDIO_VIDEO_BACKPRESSURE)) return PPU_AUDIO_AGAIN;
    return error;
}

char *mkv_decode_get_audio(struct mkv_decode_struct *p,
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
    error = ppu_audio_stream_read(p->audio_stream, mkv_audio_packet, &p->reader,
                                  audio_stream, output, &timestamp, &work_us);
    post_started = sceKernelGetSystemTimeLow();
    if (error) {
        if (!strcmp(error, PPU_AUDIO_AGAIN)) {
            error = (ppa_session_audio_only() || p->is_eof) ? PPU_AUDIO_AGAIN : MKV_READ_AUDIO_VIDEO_BACKPRESSURE;
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

char *mkv_decode_get_cached_video(struct mkv_decode_struct *p,
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
	int timestamp;
	unsigned int frame_number;
	int battery_saver_active;
	struct mkv_h264x_present_risk_struct present_meta;
	unsigned int output_source_kind = PPA_H264X_OUTPUT_SOURCE_CACHED;
	int output_detail2_index;
	int remap_restored;

	battery_saver_active = ppa_video_pipeline_extreme_battery_saver_enabled();
	remap_restored = 0;

	{
		uint64_t t_auto_decode;
		unsigned int elapsed_auto_decode;

		t_auto_decode = cpu_clock_auto_now_us();

		if (battery_saver_active) {
			result = mp4_avc_get_cache(&p->avc, ppa_gu_rgb_buffer, pic_num);
		}
		else {
			remap_restored = mkv_h264x_output_remap_restore_cached(
				p, pic_num, &output_source_kind);
			if (remap_restored < 0)
				return "mkv h264x: cached-picture restore failed";
			if (remap_restored)
				result = 0;
			else
				result = mp4_avc_get_cache(&p->avc, ppa_gu_rgb_buffer, pic_num);
		}

		elapsed_auto_decode =
			(unsigned int)(cpu_clock_auto_now_us() - t_auto_decode);

		ppa_session_note_compat(p->h264x_compat_original_refs,
                        p->h264x_compat_effective_refs, p->h264x_compat_active,
                        p->h264x_compat_disable_weights);
                cpu_clock_auto_on_decode_us(elapsed_auto_decode,
		                            p->video_frame_duration);
	}

	if (result != 0) {
		return(result);
	}

	if (!mkv_h264x_pop_presentation_pair(p, "cached", &timestamp, &present_meta))
		return "mkv_decode_get_cached_video: presentation queue invariant failed";

	output_detail2_index = remap_restored ?
		mkv_h264x_output_remap.last_physical_index :
		(p->avc.mpeg_pic_num - (int)pic_num);
	frame_number = ppa_timestamp_ms_to_frame((unsigned)timestamp,
	                                         p->reader.file.video_rate,
	                                         p->reader.file.video_scale);

	if (!battery_saver_active) {
		mkv_h264x_stamp_output_buffer(p, output_source_kind, &present_meta);

	}

	if (show_interface == 1) {
		draw_interface(p->reader.file.video_scale,
		               p->reader.file.video_rate,
		               p->reader.file.number_of_video_frames,
		               frame_number,
		               aspect_ratio,
		               zoom,
		               luminosity_boost,
		               audio_stream,
		               volume_boost,
		               loop);
	}

	mkv_present_rgb_frame(p,
	                      p->current_video_buffer_number,
	                      aspect_ratio,
	                      zoom,
	                      luminosity_boost,
	                      show_interface,
	                      show_subtitle,
	                      subtitle_format,
	                      frame_number,
	                      timestamp,
	                      "cached");

	p->output_video_frame_buffers[p->current_video_buffer_number].timestamp =
		timestamp;
	p->output_video_frame_buffers[p->current_video_buffer_number].epoch =
		p->output_epoch;

	p->last_video_timestamp = timestamp;

	p->current_video_buffer_number =
		(p->current_video_buffer_number + 1) %
		p->number_of_frame_buffers;

	return(0);
}

char *mkv_decode_get_video(struct mkv_decode_struct *p,
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
	struct mkv_read_output_struct v_packet;
	int battery_saver_active;
	unsigned int main_source_kind = PPA_H264X_OUTPUT_SOURCE_MAIN;
	int main_detail2_index = 0;

	memset(&v_packet, 0, sizeof(struct mkv_read_output_struct));
	battery_saver_active = ppa_video_pipeline_extreme_battery_saver_enabled();

	if (p->is_eof) {
		*pic_num = 1;

		in_mkv_timestamp_queue(p->timestamp_queue,
		                       &p->timestamp_queue_size,
		                       mkv_timestamp_queue_max,
		                       p->last_video_timestamp + p->video_frame_duration);
		mkv_h264x_present_queue_add_empty(p,
		                                  p->last_video_timestamp + p->video_frame_duration);
	}
	else {
		result = mkv_read_get_video(&p->reader, &v_packet);

		if (result != 0) {
			if (strcmp(result, "mkv_read_fill_buffer: eof") == 0) {
				*pic_num = 1;
				p->is_eof = 1;

				in_mkv_timestamp_queue(p->timestamp_queue,
				                       &p->timestamp_queue_size,
				                       mkv_timestamp_queue_max,
				                       p->last_video_timestamp + p->video_frame_duration);
				mkv_h264x_present_queue_add_empty(p,
				                                  p->last_video_timestamp + p->video_frame_duration);
			}
			else {
				return(result);
			}
		}
		else {
			in_mkv_timestamp_queue(p->timestamp_queue,
			                       &p->timestamp_queue_size,
			                       mkv_timestamp_queue_max,
			                       v_packet.timestamp);

			if (p->video_format == 0x61766331 /* avc1 */) {
				int avc_mode;
				
				avc_mode = p->avc_first_packet_done ? 0 : 3;
				p->avc_first_packet_done = 1;

				if (battery_saver_active) {
					uint64_t t_auto_decode;
					unsigned int elapsed_auto_decode;

					/* Native-only battery mode deliberately bypasses all H.264X
					 * AU probing, rewriting, DPB repair and output remapping. Keep
					 * exactly one metadata entry beside the timestamp queue so the
					 * ordinary cached-picture ordering remains unchanged. */
					mkv_h264x_present_queue_add_empty(p, v_packet.timestamp);
					ppa_cache_cpu_wrote_me_will_read(v_packet.data, v_packet.size);
					t_auto_decode = cpu_clock_auto_now_us();
					result = mp4_avc_get(&p->avc, avc_mode,
					                     v_packet.data, v_packet.size,
					                     ppa_gu_rgb_buffer, pic_num);
					elapsed_auto_decode =
						(unsigned int)(cpu_clock_auto_now_us() - t_auto_decode);
					ppa_session_note_compat(p->h264x_compat_original_refs,
                        p->h264x_compat_effective_refs, p->h264x_compat_active,
                        p->h264x_compat_disable_weights);
                cpu_clock_auto_on_decode_us(elapsed_auto_decode,
					                            p->video_frame_duration);
				}
				else {
					uint64_t t_auto_decode;
					unsigned int elapsed_auto_decode;
					struct h264x_rplm_override dual_plan;

					t_auto_decode = cpu_clock_auto_now_us();
					memset(&dual_plan, 0, sizeof(dual_plan));

					h264x_probe_before_decode(&p->h264x,
					                         v_packet.data,
					                         v_packet.size,
					                         v_packet.timestamp,
					                         avc_mode);
					mkv_h264x_deep_b_observe(p, v_packet.timestamp);
					mkv_h264x_dual_prepare(p, &dual_plan);
					
					mkv_h264x_present_queue_add_probe(p, &p->h264x, v_packet.timestamp);

					/*
					 * B-pyramid compatibility bridge: once the probe has identified the
					 * ref-B / ref-management pattern, pre-rewrite before Sony sees the AU.
					 * This avoids letting a failed sceMpegAvcDecode partially dirty native
					 * decoder state before the compatible retry.
					 */
					{
						void *decode_buffer;
						unsigned int decode_size;
						void *pre_rewrite_buffer;
						unsigned int pre_rewrite_size;
						unsigned int pre_rewrite_capacity;
						int pre_rewrite_used;
						unsigned int rewrite_flags;
						int weights_rewrite_required;
						int dual_rplm_rewrite_needed;

						decode_buffer = v_packet.data;
						decode_size = v_packet.size;
						pre_rewrite_buffer = 0;
						pre_rewrite_size = 0;
						pre_rewrite_capacity = 0;
						pre_rewrite_used = 0;
						weights_rewrite_required = p->h264x_compat_active &&
							!p->h264x_compat_use_original_ps &&
							p->h264x_compat_disable_weights &&
							h264x_rewrite_weights_required(&p->h264x);
						/* MP4 and MKV now share the shallow MMCO/RPLM/weight rule.
						 * MKV retains its existing deep-DPB override as an input. */
						rewrite_flags = h264x_ref_bridge_flags(&p->h264x,
							p->h264x_compat_effective_refs,
							p->h264x_compat_flatten_brefs,
							mkv_h264x_deep_b_preserve_rplm(), weights_rewrite_required);
						dual_rplm_rewrite_needed = dual_plan.enabled ? 1 : 0;
						if (dual_plan.enabled || dual_plan.fallback_to_default)
							rewrite_flags |= H264X_REWRITE_RPLM | H264X_REWRITE_MMCO;

						if (p->h264x.current_au.nal_unit_type == 5 &&
						    p->h264x.current_au.au_index > 1U) {
							avc_mode = 3;
							
						}

#if PPA_H264X_REWRITE_RETRY && PPA_H264X_PRE_REWRITE
						
						if (v_packet.data != 0 &&
						    v_packet.size != 0 &&
						    v_packet.size <= 0xffffffffU - 512U &&
						    rewrite_flags != 0U &&
						    !p->h264x.current_au.uses_8x8_transform) {
							pre_rewrite_capacity = v_packet.size + 512;
							pre_rewrite_buffer = malloc_64(pre_rewrite_capacity);
							if (pre_rewrite_buffer != 0 &&
							    (dual_rplm_rewrite_needed ?
							     h264x_rewrite_ref_mgmt_override(&p->h264x,
							                                      v_packet.data,
							                                      v_packet.size,
							                                      pre_rewrite_buffer,
							                                      pre_rewrite_capacity,
							                                      &pre_rewrite_size,
							                                      rewrite_flags, &dual_plan) :
							     h264x_rewrite_ref_mgmt_select(&p->h264x,
							                                   v_packet.data,
							                                   v_packet.size,
							                                   pre_rewrite_buffer,
							                                   pre_rewrite_capacity,
							                                   &pre_rewrite_size,
							                                   rewrite_flags))) {
								decode_buffer = pre_rewrite_buffer;
								decode_size = pre_rewrite_size;
								pre_rewrite_used = 1;
								if (dual_rplm_rewrite_needed) {
									mkv_h264x_dual_rewrites++;
									
								}
#if PPA_H264X_REWRITE_FLUSH
								/* Publish only the rewritten AU bytes consumed by the Sony decoder. */
								ppa_cache_cpu_wrote_me_will_read(decode_buffer, decode_size);
#endif

							}
						}
#endif

						/* The decoder-facing PPS no longer signals a weight table.
						 * An allocation/parser failure must not submit source syntax
						 * under that PPS or partially convert a multi-slice picture. */
						if (weights_rewrite_required && !pre_rewrite_used) {
							if (pre_rewrite_buffer != 0)
								free_64(pre_rewrite_buffer);
							mkv_read_release_output(&v_packet);
							return "h264x: cannot safely remove explicit weights";
						}
						result = mp4_avc_get(&p->avc,
						                    avc_mode,
						                    decode_buffer,
						                    decode_size,
						                    ppa_gu_rgb_buffer,
						                    pic_num);

						/* A first-IDR failure is the only point where capability retries are
						 * state-safe.  Escalate without changing CABAC: preserve prediction
						 * first, then remove weighting, normalize level, and only finally
						 * flatten B references under a one-reference decoder envelope. */
						if (result != 0 &&
						    
						    p->h264x_compat_active &&
						    p->h264x.current_au.au_index == 1U &&
						    p->h264x.current_au.is_idr &&
						    !p->h264x_compat_first_idr_retry_attempted) {
							char *retry_result;
							int retry_pic_num;
							p->h264x_compat_first_idr_retry_attempted = 1U;
							retry_result = result;
							retry_pic_num = 0;

							/* Stage 1: retain CABAC, weighting and reference-B semantics;
							 * only normalize the decoder-facing SPS DPB count. */
							p->h264x_compat_use_original_ps = 0U;
							p->h264x_compat_retry_stage = 1U;
							retry_result = mkv_h264x_retry_first_idr_stage(
								p, decode_buffer, decode_size, 0U,
								p->h264x_compat_effective_refs,
								"normalized_dpb", &retry_pic_num);

							/* Some contexts reject refs=3 at the first IDR. Fall back to
							 * refs=2 only after testing the three-reference working set. */
							if (retry_result != 0 &&
							    p->h264x_compat_effective_refs > 2U) {
								p->h264x_compat_retry_stage = 2U;
								retry_result = mkv_h264x_retry_first_idr_stage(
									p, decode_buffer, decode_size, 0U, 2U,
									"normalized_refs2", &retry_pic_num);
							}

							if (retry_result != 0 &&
							    p->h264x_compat_mpeg_mode == 5U) {
								p->h264x_compat_mpeg_mode = 4U;
								p->h264x_compat_retry_stage = 3U;
								retry_result = mkv_h264x_retry_first_idr_stage(
									p, decode_buffer, decode_size, 0U,
									p->h264x_compat_effective_refs,
									"normalized_mode4", &retry_pic_num);
							}

							if (retry_result != 0 &&
							    (p->h264x_compat_flags &
							     PPA_H264X_COMPAT_WEIGHTED_PRED) != 0U &&
							    !p->h264x_compat_disable_weights) {
								p->h264x_compat_disable_weights = 1U;
								p->h264x_compat_retry_stage = 4U;
								retry_result = mkv_h264x_retry_first_idr_stage(
									p, decode_buffer, decode_size, 0U,
									p->h264x_compat_effective_refs,
									"weights_off", &retry_pic_num);
								
							}

							if (retry_result != 0 && p->h264x.sps.level_idc != 30U) {
								p->h264x_compat_flags |= PPA_H264X_COMPAT_LEVEL_RETRY;
								p->h264x_compat_retry_stage = 5U;
								retry_result = mkv_h264x_retry_first_idr_stage(
									p, decode_buffer, decode_size, 30U,
									p->h264x_compat_effective_refs,
									"level30", &retry_pic_num);
							}

							if (retry_result != 0) {
								p->h264x_compat_flags |= PPA_H264X_COMPAT_LEVEL_RETRY;
								p->h264x_compat_flatten_brefs = 1U;
								p->h264x_compat_retry_stage = 6U;
								retry_result = mkv_h264x_retry_first_idr_stage(
									p, decode_buffer, decode_size, 30U, 1U,
									"strict_refs1", &retry_pic_num);
							}

							if (retry_result == 0) {
								result = 0;
								*pic_num = retry_pic_num;
							}
						}

#if PPA_H264X_REWRITE_RETRY

						if (result != 0 &&
						    pre_rewrite_used &&
						    ((rewrite_flags & H264X_REWRITE_DROP_REF) != 0) &&
						    v_packet.data != 0 &&
						    v_packet.size != 0 &&
						    v_packet.size <= 0xffffffffU - 512U) {
							void *restore_buffer;
							unsigned int restore_size;
							unsigned int restore_capacity;
							unsigned int restore_flags;

							restore_capacity = v_packet.size + 512;
							restore_buffer = malloc_64(restore_capacity);
							restore_size = 0;
							restore_flags = rewrite_flags & ~H264X_REWRITE_DROP_REF;

							if (restore_buffer != 0 &&
							    h264x_rewrite_ref_mgmt_select(&p->h264x,
							                                  v_packet.data,
							                                  v_packet.size,
							                                  restore_buffer,
							                                  restore_capacity,
							                                  &restore_size,
							                                  restore_flags)) {
								char *restore_result;
								int restore_pic_num;

								restore_pic_num = 0;
#if PPA_H264X_REWRITE_FLUSH
								ppa_cache_cpu_wrote_me_will_read(restore_buffer, restore_size);
#endif

								;
								restore_result = mp4_avc_get(&p->avc,
								                             avc_mode,
								                             restore_buffer,
								                             restore_size,
								                             ppa_gu_rgb_buffer,
								                             &restore_pic_num);

								if (restore_result == 0) {
									result = 0;
									*pic_num = restore_pic_num;
								}
							}

							if (restore_buffer != 0)
								free_64(restore_buffer);
						}

						if (result != 0 &&
						    PPA_H264X_LEGACY_REF_MGMT_REWRITE &&
						    !pre_rewrite_used &&
						    v_packet.data != 0 &&
						    v_packet.size != 0 &&
						    v_packet.size <= 0xffffffffU - 512U &&
						    p->h264x.reference_b_count != 0 &&
						    (p->h264x.current_au.is_p ||
						     p->h264x.current_au.is_b ||
						     (PPA_H264X_REWRITE_I_MMCO && p->h264x.current_au.is_i && p->h264x.current_au.mmco_nonzero)) &&
						    (p->h264x.current_au.ref_pic_list_modification_flag_l0 ||
						     p->h264x.current_au.ref_pic_list_modification_flag_l1 ||
						     p->h264x.current_au.mmco_nonzero)) {
							void *rewrite_buffer;
							unsigned int rewrite_size;
							unsigned int rewrite_capacity;

							rewrite_capacity = v_packet.size + 512;
							rewrite_buffer = malloc_64(rewrite_capacity);
							rewrite_size = 0;

							if (rewrite_buffer != 0 &&
							    h264x_rewrite_ref_mgmt_select(&p->h264x,
							                                      v_packet.data,
							                                      v_packet.size,
							                                      rewrite_buffer,
							                                      rewrite_capacity,
							                                      &rewrite_size,
							                                      rewrite_flags)) {
								char *retry_result;
								int retry_pic_num;

								retry_pic_num = 0;
#if PPA_H264X_REWRITE_FLUSH
								ppa_cache_cpu_wrote_me_will_read(rewrite_buffer, rewrite_size);
#endif

								;
								retry_result = mp4_avc_get(&p->avc,
								                           avc_mode,
								                           rewrite_buffer,
								                           rewrite_size,
								                           ppa_gu_rgb_buffer,
								                           &retry_pic_num);

								if (retry_result == 0) {
									result = 0;
									*pic_num = retry_pic_num;
								}
							}

							if (rewrite_buffer != 0)
								free_64(rewrite_buffer);
						}
#endif

						mkv_h264x_reorder_note_decode(p, result, *pic_num, v_packet.timestamp);

						if (pre_rewrite_buffer != 0)
							free_64(pre_rewrite_buffer);
					}

					/* One governor sample includes probing, rewriting and every retry. */
					elapsed_auto_decode =
						(unsigned int)(cpu_clock_auto_now_us() - t_auto_decode);

					ppa_session_note_compat(p->h264x_compat_original_refs,
                        p->h264x_compat_effective_refs, p->h264x_compat_active,
                        p->h264x_compat_disable_weights);
                cpu_clock_auto_on_decode_us(elapsed_auto_decode,
												p->video_frame_duration);
				}
			}

			if (v_packet.data) {
				mkv_read_release_output(&v_packet);
			}

			if (result != 0) {
				return(result);
			}
		}
	}

	if (!battery_saver_active &&
	    p->video_format == 0x61766331 && *pic_num >= 2) {
		main_source_kind = mkv_h264x_output_remap_prepare_main(p, *pic_num);
		if (main_source_kind == PPA_H264X_OUTPUT_SOURCE_REMAP_ERROR)
			return "mkv h264x: two-picture recovery failed";
		if (main_source_kind == PPA_H264X_OUTPUT_SOURCE_REMAP_MAIN)
			main_detail2_index = 1;
		else if (main_source_kind == PPA_H264X_OUTPUT_SOURCE_BURST_MAIN ||
		         main_source_kind == PPA_H264X_OUTPUT_SOURCE_LEDGER_MAIN)
			main_detail2_index = *pic_num - 1;
	}

	*pic_num = *pic_num - 1;

	if (*pic_num >= 0) {
		int timestamp;
		unsigned int frame_number;
		struct mkv_h264x_present_risk_struct present_meta;

		if (!mkv_h264x_pop_presentation_pair(p, "main", &timestamp, &present_meta)) {
			return "mkv_decode_get_video: presentation queue invariant failed";
		}

		frame_number = ppa_timestamp_ms_to_frame((unsigned)timestamp,
		                                         p->reader.file.video_rate,
		                                         p->reader.file.video_scale);

		if (!battery_saver_active) {
			mkv_h264x_stamp_output_buffer(p, main_source_kind, &present_meta);

		}

		if (show_interface == 1) {
			draw_interface(p->reader.file.video_scale,
			               p->reader.file.video_rate,
			               p->reader.file.number_of_video_frames,
			               frame_number,
			               aspect_ratio,
			               zoom,
			               luminosity_boost,
			               audio_stream,
			               volume_boost,
			               loop);
		}

		mkv_present_rgb_frame(p,
		                      p->current_video_buffer_number,
		                      aspect_ratio,
		                      zoom,
		                      luminosity_boost,
		                      show_interface,
		                      show_subtitle,
		                      subtitle_format,
		                      frame_number,
		                      timestamp,
		                      "main");

		p->output_video_frame_buffers[p->current_video_buffer_number].timestamp =
			timestamp;
		p->output_video_frame_buffers[p->current_video_buffer_number].epoch =
			p->output_epoch;

		p->last_video_timestamp = timestamp;

		p->current_video_buffer_number =
			(p->current_video_buffer_number + 1) %
			p->number_of_frame_buffers;

	}

	return(0);
}
