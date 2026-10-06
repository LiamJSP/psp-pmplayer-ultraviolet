/*
 * h264x_dpb.c - H.264 shadow DPB / reference-state model.
 *
 * This is intentionally conservative.  It tracks enough of the intended DPB,
 * RPLM, and MMCO state to identify frames that require a custom reconstruction
 * path while leaving the native Sony decode path untouched.
 */

#include "h264x_dpb.h"
#include "h264x_probe.h"

#include <stdio.h>
#include <string.h>

static unsigned int h264x_dpb_clamp_refs(unsigned int n)
{
	if (n == 0)
		return 1;
	if (n > H264X_DPB_MAX_REFS)
		return H264X_DPB_MAX_REFS;
	return n;
}

static void h264x_dpb_recount(struct h264x_dpb *dpb)
{
	unsigned int i;

	dpb->ref_count = 0;
	dpb->short_count = 0;
	dpb->long_count = 0;

	for (i = 0; i < H264X_DPB_MAX_REFS; i++) {
		if (!dpb->refs[i].valid)
			continue;
		dpb->ref_count++;
		if (dpb->refs[i].long_term)
			dpb->long_count++;
		else
			dpb->short_count++;
	}
}

void h264x_dpb_init(struct h264x_dpb *dpb)
{
	if (dpb == 0)
		return;
	memset(dpb, 0, sizeof(*dpb));
	dpb->enabled = 1;
	dpb->max_refs = 1;
	dpb->max_frame_num = 16;
	dpb->max_long_term_frame_idx = -1;
}

void h264x_dpb_reset(struct h264x_dpb *dpb)
{
	unsigned int max_refs;
	unsigned int max_frame_num;
	int max_ltf;
	int enabled;

	if (dpb == 0)
		return;

	max_refs = dpb->max_refs;
	max_frame_num = dpb->max_frame_num;
	max_ltf = dpb->max_long_term_frame_idx;
	enabled = dpb->enabled;
	memset(dpb, 0, sizeof(*dpb));
	dpb->enabled = enabled ? enabled : 1;
	dpb->max_refs = max_refs ? max_refs : 1;
	dpb->max_frame_num = max_frame_num ? max_frame_num : 16;
	dpb->max_long_term_frame_idx = max_ltf;
}

void h264x_dpb_configure(struct h264x_dpb *dpb, const struct h264x_sps_info *sps)
{
	unsigned int bits;

	if (dpb == 0)
		return;

	if (sps != 0 && sps->valid) {
		dpb->max_refs = h264x_dpb_clamp_refs(sps->num_ref_frames);
		bits = sps->log2_max_frame_num_minus4 + 4U;
		if (bits >= 16U)
			dpb->max_frame_num = 1U << 16;
		else
			dpb->max_frame_num = 1U << bits;
	}
	else {
		dpb->max_refs = 1;
		dpb->max_frame_num = 16;
	}
	if (dpb->max_frame_num == 0)
		dpb->max_frame_num = 16;
	if (dpb->max_long_term_frame_idx < -1)
		dpb->max_long_term_frame_idx = -1;
}

static int h264x_dpb_poc(const struct h264x_au_info *au)
{
	return (int)au->pic_order_cnt_lsb;
}

static int h264x_dpb_find_empty(const struct h264x_dpb *dpb)
{
	unsigned int i;
	for (i = 0; i < H264X_DPB_MAX_REFS; i++) {
		if (!dpb->refs[i].valid)
			return (int)i;
	}
	return -1;
}

static int h264x_dpb_find_oldest_short(const struct h264x_dpb *dpb)
{
	unsigned int i;
	int best = -1;
	unsigned int best_au = 0xffffffffU;

	for (i = 0; i < H264X_DPB_MAX_REFS; i++) {
		if (!dpb->refs[i].valid || !dpb->refs[i].short_term)
			continue;
		if (best < 0 || dpb->refs[i].au_index < best_au) {
			best = (int)i;
			best_au = dpb->refs[i].au_index;
		}
	}
	return best;
}

static int h264x_dpb_find_long(const struct h264x_dpb *dpb, unsigned int long_idx)
{
	unsigned int i;
	for (i = 0; i < H264X_DPB_MAX_REFS; i++) {
		if (dpb->refs[i].valid && dpb->refs[i].long_term &&
		    dpb->refs[i].long_term_frame_idx == long_idx)
			return (int)i;
	}
	return -1;
}

static int h264x_dpb_find_short_frame(const struct h264x_dpb *dpb, unsigned int frame_num)
{
	unsigned int i;
	int best = -1;
	unsigned int best_au = 0;

	for (i = 0; i < H264X_DPB_MAX_REFS; i++) {
		if (!dpb->refs[i].valid || !dpb->refs[i].short_term)
			continue;
		if (dpb->refs[i].frame_num == frame_num) {
			if (best < 0 || dpb->refs[i].au_index > best_au) {
				best = (int)i;
				best_au = dpb->refs[i].au_index;
			}
		}
	}
	return best;
}

static void h264x_dpb_remove(struct h264x_dpb *dpb, int idx)
{
	if (idx < 0 || idx >= (int)H264X_DPB_MAX_REFS)
		return;
	memset(&dpb->refs[idx], 0, sizeof(dpb->refs[idx]));
}

static void h264x_dpb_clear_refs(struct h264x_dpb *dpb)
{
	memset(dpb->refs, 0, sizeof(dpb->refs));
	dpb->clears++;
	dpb->ref_count = 0;
	dpb->short_count = 0;
	dpb->long_count = 0;
}

static void h264x_dpb_apply_long_limit(struct h264x_dpb *dpb, int max_ltf)
{
	unsigned int i;

	dpb->max_long_term_frame_idx = max_ltf;
	for (i = 0; i < H264X_DPB_MAX_REFS; i++) {
		if (!dpb->refs[i].valid || !dpb->refs[i].long_term)
			continue;
		if (max_ltf < 0 || dpb->refs[i].long_term_frame_idx > (unsigned int)max_ltf)
			h264x_dpb_remove(dpb, (int)i);
	}
}

static unsigned int h264x_dpb_rplm_target_frame(const struct h264x_dpb *dpb,
                                                unsigned int *pic_num_pred,
                                                const struct h264x_rplm_op *op)
{
	unsigned int delta;
	unsigned int mod;
	unsigned int pred;

	mod = dpb->max_frame_num ? dpb->max_frame_num : 16;
	pred = pic_num_pred ? (*pic_num_pred % mod) : 0U;
	delta = op->abs_diff_pic_num_minus1 + 1U;

	/* H.264 8.2.4.3: picNumPred is cumulative within each list.  The old
	 * model incorrectly restarted every operation at CurrPicNum, producing
	 * false RPLM misses on perfectly valid pyramids. This model still records
	 * modulo frame_num (not full FrameNumWrap), but now follows the operation
	 * chain correctly. */
	if (op->modification_of_pic_nums_idc == 0)
		pred = (pred + mod - (delta % mod)) % mod;
	else if (op->modification_of_pic_nums_idc == 1)
		pred = (pred + delta) % mod;

	if (pic_num_pred)
		*pic_num_pred = pred;
	return pred;
}

static void h264x_dpb_reset_last_au(struct h264x_dpb *dpb,
                                    const struct h264x_au_info *au)
{
	dpb->last_au_index = au ? au->au_index : 0;
	dpb->last_rplm_ops = 0;
	dpb->last_rplm_found = 0;
	dpb->last_rplm_missing = 0;
	dpb->last_rplm_bref_refs = 0;
	dpb->last_mmco_ops = 0;
	dpb->last_mmco_found = 0;
	dpb->last_mmco_absent = 0;
	dpb->last_mmco_benign_absent = 0;
	dpb->last_mmco_true_missing = 0;
	dpb->last_visible_risk = 0;
	dpb->last_risk_class = 0;
}

static void h264x_dpb_analyze_rplm(struct h264x_dpb *dpb,
                                   const struct h264x_au_info *au)
{
	unsigned int list;
	unsigned int i;
	const struct h264x_rplm_op *ops;
	unsigned int count;

	if (dpb == 0 || au == 0)
		return;

	for (list = 0; list < 2; list++) {
		unsigned int pic_num_pred =
			au->frame_num % (dpb->max_frame_num ? dpb->max_frame_num : 16U);
		if (list == 0) {
			ops = au->rplm_l0;
			count = au->rplm_l0_count;
		}
		else {
			ops = au->rplm_l1;
			count = au->rplm_l1_count;
		}

		for (i = 0; i < count; i++) {
			int idx = -1;
			unsigned int idc = ops[i].modification_of_pic_nums_idc;

			dpb->last_rplm_ops++;
			if (idc == 0 || idc == 1) {
				unsigned int target_frame;
				target_frame = h264x_dpb_rplm_target_frame(
					dpb, &pic_num_pred, &ops[i]);
				idx = h264x_dpb_find_short_frame(dpb, target_frame);
			}
			else if (idc == 2) {
				idx = h264x_dpb_find_long(dpb, ops[i].long_term_pic_num);
			}

			if (idx >= 0) {
				dpb->last_rplm_found++;
				if (dpb->refs[idx].is_reference_b)
					dpb->last_rplm_bref_refs++;
			}
			else {
				dpb->last_rplm_missing++;
			}
		}
	}
}

static int h264x_dpb_mmco_absent_is_benign(const struct h264x_dpb *dpb,
                                           const struct h264x_au_info *au,
                                           const struct h264x_mmco_op *op,
                                           unsigned int target)
{
	(void)dpb;
	(void)target;

	if (au == 0 || op == 0)
		return 0;

	/*
	 * The E sample repeatedly carries MMCO op=1 on reference-B slices for
	 * pictures that have already been removed by the preceding P-reference
	 * transition.  Counting that as visible drift made the repair scheduler
	 * noisy.  Keep it as an observed absent target, but do not promote it to
	 * true-missing unless a P/I reference management AU loses its target.
	 */
	if (au->is_reference_b &&
	    op->memory_management_control_operation == 1)
		return 1;

	return 0;
}

static void h264x_dpb_note_mmco_target(struct h264x_dpb *dpb,
                                       const struct h264x_au_info *au,
                                       const struct h264x_mmco_op *op,
                                       unsigned int target,
                                       int found)
{
	if (dpb == 0)
		return;

	if (found) {
		dpb->last_mmco_found++;
		return;
	}

	dpb->last_mmco_absent++;
	dpb->mmco_absent_events++;
	if (h264x_dpb_mmco_absent_is_benign(dpb, au, op, target)) {
		dpb->last_mmco_benign_absent++;
		dpb->mmco_benign_absent_events++;
	}
	else {
		dpb->last_mmco_true_missing++;
		dpb->true_missing_ref_events++;
		dpb->missing_ref_events = dpb->true_missing_ref_events;
	}
}

static void h264x_dpb_classify_risk(struct h264x_dpb *dpb,
                                    const struct h264x_au_info *au)
{
	unsigned int risk = 0;
	unsigned int klass = 0;

	if (dpb == 0 || au == 0)
		return;

	if (au->mmco_nonzero) {
		klass |= H264X_DPB_RISK_MMCO_STRIPPED;
		risk += 1;
	}
	if (au->is_reference_b) {
		klass |= H264X_DPB_RISK_REF_B;
		risk += au->mmco_nonzero ? 4U : 1U;
	}
	if (au->is_p && (au->mmco_nonzero || dpb->last_rplm_ops != 0)) {
		klass |= H264X_DPB_RISK_P_REF_MGMT;
		risk += 2;
	}
	if (dpb->last_rplm_found != 0) {
		klass |= H264X_DPB_RISK_RPLM_FOUND;
		risk += 1;
	}
	if (dpb->last_rplm_missing != 0) {
		klass |= H264X_DPB_RISK_RPLM_MISSING;
		risk += 4U * dpb->last_rplm_missing;
	}
	if (dpb->last_mmco_true_missing != 0) {
		klass |= H264X_DPB_RISK_TRUE_MISSING;
		risk += 5U * dpb->last_mmco_true_missing;
	}
	if (au->au_size >= 2048U && (au->mmco_nonzero || dpb->last_rplm_ops != 0)) {
		klass |= H264X_DPB_RISK_LARGE_AU;
		risk += 2;
	}
	if (au->is_b && au->direct_spatial_mv_pred_flag && !au->is_reference) {
		klass |= H264X_DPB_RISK_DIRECT_B;
		/* Direct non-ref B pictures are often where native-reference drift shows visually. */
		if (dpb->last_visible_risk != 0)
			risk += 1;
	}

	dpb->last_visible_risk = risk;
	dpb->last_risk_class = klass;
	if (risk != 0) {
		dpb->visible_risk_events++;
		if (au->is_reference_b)
			dpb->risky_bref_events++;
		if (au->is_p)
			dpb->risky_pref_events++;
	}
}

static int h264x_dpb_apply_mmco(struct h264x_dpb *dpb,
                                const struct h264x_au_info *au,
                                unsigned int *current_long_idx)
{
	unsigned int i;
	int force_long = 0;
	unsigned int mod;

	mod = dpb->max_frame_num ? dpb->max_frame_num : 16;
	if (current_long_idx)
		*current_long_idx = 0;

	for (i = 0; i < au->mmco_op_count; i++) {
		const struct h264x_mmco_op *op = &au->mmco_ops[i];
		unsigned int mmco = op->memory_management_control_operation;
		int idx;

		if (mmco == 0)
			continue;
		dpb->mmco_events++;
		dpb->last_mmco_ops++;

		switch (mmco) {
		case 1: {
			unsigned int delta = op->difference_of_pic_nums_minus1 + 1U;
			unsigned int target = (au->frame_num + mod - (delta % mod)) % mod;
			idx = h264x_dpb_find_short_frame(dpb, target);
			h264x_dpb_note_mmco_target(dpb, au, op, target, idx >= 0);
			if (idx >= 0)
				h264x_dpb_remove(dpb, idx);
			
			break;
		}
		case 2:
			idx = h264x_dpb_find_long(dpb, op->long_term_pic_num);
			h264x_dpb_note_mmco_target(dpb, au, op, op->long_term_pic_num, idx >= 0);
			if (idx >= 0)
				h264x_dpb_remove(dpb, idx);
			
			break;
		case 3: {
			unsigned int delta = op->difference_of_pic_nums_minus1 + 1U;
			unsigned int target = (au->frame_num + mod - (delta % mod)) % mod;
			idx = h264x_dpb_find_short_frame(dpb, target);
			h264x_dpb_note_mmco_target(dpb, au, op, target, idx >= 0);
			if (idx >= 0) {
				int old_long = h264x_dpb_find_long(dpb, op->long_term_frame_idx);
				if (old_long >= 0 && old_long != idx)
					h264x_dpb_remove(dpb, old_long);
				dpb->refs[idx].short_term = 0;
				dpb->refs[idx].long_term = 1;
				dpb->refs[idx].long_term_frame_idx = op->long_term_frame_idx;
			}
			
			break;
		}
		case 4:
			if (op->max_long_term_frame_idx_plus1 == 0)
				h264x_dpb_apply_long_limit(dpb, -1);
			else
				h264x_dpb_apply_long_limit(dpb, (int)op->max_long_term_frame_idx_plus1 - 1);
			break;
		case 5:
			h264x_dpb_clear_refs(dpb);
			break;
		case 6:
			force_long = 1;
			if (current_long_idx)
				*current_long_idx = op->long_term_frame_idx;
			break;
		default:
			break;
		}
	}

	return force_long;
}

static void h264x_dpb_add_current(struct h264x_dpb *dpb,
                                  const struct h264x_au_info *au,
                                  int make_long,
                                  unsigned int long_idx)
{
	int idx;
	int evict;
	struct h264x_dpb_ref *r;

	if (!au->is_reference)
		return;

	/* Counts may be stale after MMCO/removal. Recount exactly once here and
	 * make this function the sole sliding-window eviction owner. The old
	 * caller and callee both evicted, dropping two references at capacity. */
	h264x_dpb_recount(dpb);
	if (make_long) {
		int old_long = h264x_dpb_find_long(dpb, long_idx);
		if (old_long >= 0)
			h264x_dpb_remove(dpb, old_long);
	}
	else if (dpb->ref_count >= dpb->max_refs) {
		evict = h264x_dpb_find_oldest_short(dpb);
		if (evict >= 0)
			h264x_dpb_remove(dpb, evict);
	}
	h264x_dpb_recount(dpb);

	idx = h264x_dpb_find_empty(dpb);
	if (idx < 0) {
		evict = h264x_dpb_find_oldest_short(dpb);
		if (evict < 0)
			evict = 0;
		h264x_dpb_remove(dpb, evict);
		idx = evict;
	}

	r = &dpb->refs[idx];
	memset(r, 0, sizeof(*r));
	r->valid = 1;
	r->short_term = !make_long;
	r->long_term = make_long;
	r->is_reference_b = au->is_reference_b;
	r->frame_num = au->frame_num;
	r->frame_num_wrap = (int)au->frame_num;
	r->poc = h264x_dpb_poc(au);
	r->poc_lsb = au->pic_order_cnt_lsb;
	r->long_term_frame_idx = long_idx;
	r->au_index = au->au_index;
	r->timestamp = au->timestamp;
	r->nal_ref_idc = au->nal_ref_idc;

	if (au->is_reference_b)
		dpb->intended_brefs++;

}

void h264x_dpb_observe_au(struct h264x_dpb *dpb,
                          const struct h264x_sps_info *sps,
                          const struct h264x_au_info *au)
{
	unsigned int current_long_idx = 0;
	int force_long = 0;

	if (dpb == 0 || au == 0 || !dpb->enabled)
		return;

	h264x_dpb_configure(dpb, sps);

	if (au->vcl_count == 0)
		return;

	h264x_dpb_reset_last_au(dpb, au);

	if (au->ref_pic_list_modification_flag_l0 || au->ref_pic_list_modification_flag_l1)
		dpb->rplm_events++;

	h264x_dpb_analyze_rplm(dpb, au);

	if (au->is_idr) {
		h264x_dpb_clear_refs(dpb);
		dpb->last_idr_au = au->au_index;
		if (au->is_reference) {
			if (au->long_term_reference_flag) {
				force_long = 1;
				current_long_idx = 0;
			}
			h264x_dpb_add_current(dpb, au, force_long, current_long_idx);
		}
		h264x_dpb_recount(dpb);
	}
	else {
		if (au->is_reference) {
			if (au->adaptive_ref_pic_marking_mode_flag)
				force_long = h264x_dpb_apply_mmco(dpb, au, &current_long_idx);
			h264x_dpb_add_current(dpb, au, force_long, current_long_idx);
			h264x_dpb_recount(dpb);
		}
	}

	h264x_dpb_classify_risk(dpb, au);

}

