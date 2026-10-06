

#ifndef __PPA_H264X_REWRITE_H__
#define __PPA_H264X_REWRITE_H__

#include <stdint.h>
#include "h264x_probe.h"

#ifndef PPA_H264X_REWRITE_RETRY
#define PPA_H264X_REWRITE_RETRY 0
#endif

#define PPA_H264X_REWRITE_ENABLED \
    (PPA_H264X_REWRITE_RETRY)

/* Reported weighted streams open successfully yet corrupt isolated pictures.
 * Keep the source PPS immutable; this controls only the decoder-facing copy.
 * Setting 1 restores weights-preserving first attempts for comparison. */
#ifndef PPA_H264X_PRESERVE_WEIGHTS
#define PPA_H264X_PRESERVE_WEIGHTS 0
#endif

#ifndef PPA_H264X_PRE_REWRITE
#define PPA_H264X_PRE_REWRITE 0
#endif

#ifndef PPA_H264X_REWRITE_I_MMCO
#define PPA_H264X_REWRITE_I_MMCO 1
#endif

#ifndef PPA_H264X_REWRITE_FLUSH
#define PPA_H264X_REWRITE_FLUSH 1
#endif

#define H264X_REWRITE_RPLM 1U
#define H264X_REWRITE_MMCO 2U
#define H264X_REWRITE_DROP_REF 4U
#define H264X_REWRITE_WEIGHT_TABLE 8U
#define H264X_REWRITE_ALL  (H264X_REWRITE_RPLM | H264X_REWRITE_MMCO | H264X_REWRITE_DROP_REF)
#define H264X_REWRITE_ANY  (H264X_REWRITE_ALL | H264X_REWRITE_WEIGHT_TABLE)

/* 6.3.47A optional explicit ref_pic_list_modification replacement.
 * The caller supplies complete list prefixes terminated by the writer with
 * modification_of_pic_nums_idc == 3.  This changes slice-header list identity
 * only; compressed macroblock data, MMCO and AU order are untouched. */
#define H264X_RPLM_OVERRIDE_MAX_OPS 16U
struct h264x_rplm_override {
	unsigned int enabled;
	/* The source list cannot be represented in the reduced Sony DPB. In this
	 * case rewrite RPLM to the decoder's default list rather than submitting a
	 * known-unresolvable custom list that can hang sceMpegAvcDecode. */
	unsigned int fallback_to_default;
	unsigned int l0_count;
	unsigned int l1_count;
	struct h264x_rplm_op l0[H264X_RPLM_OVERRIDE_MAX_OPS];
	struct h264x_rplm_op l1[H264X_RPLM_OVERRIDE_MAX_OPS];
};

#ifdef __cplusplus
extern "C" {
#endif

/* Target the reported strict B3 / three-reference, 4x4 cases. Do not extend
 * this policy to the paused deep-DPB, field or High 8x8 experiments. Both
 * adapters need pre-submission conversion before disabling explicit weights. */
static inline int h264x_disable_weights_on_open(const struct h264x_probe *probe)
{
#if PPA_H264X_REWRITE_ENABLED && PPA_H264X_PRE_REWRITE && !PPA_H264X_PRESERVE_WEIGHTS
	return probe != 0 && probe->avcc_valid && probe->sps.valid && probe->pps.valid &&
	       probe->sps.frame_mbs_only_flag &&
	       !probe->sps.separate_colour_plane_flag &&
	       probe->sps.chroma_format_idc == 1U &&
	       probe->sps.bit_depth_luma_minus8 == 0U &&
	       probe->sps.bit_depth_chroma_minus8 == 0U &&
	       probe->sps.num_ref_frames > 0U && probe->sps.num_ref_frames <= 3U &&
	       probe->pps.num_slice_groups_minus1 == 0U &&
	       probe->pps.seq_parameter_set_id == probe->sps.seq_parameter_set_id &&
	       !probe->pps.transform_8x8_mode_flag &&
	       (probe->pps.weighted_pred_flag || probe->pps.weighted_bipred_idc != 0U);
#else
	(void)probe;
	return 0;
#endif
}

/* Removing weights does not change macroblock ref_idx values. Weighted P
 * encodes may reorder or repeat a reference, so retain their list even in the
 * lossy weights-off path when the original reference budget is still intact.
 * A reduced/flattened DPB still needs the existing compatibility fallback. */
static inline int h264x_preserve_weighted_p_lists(const struct h264x_probe *probe,
                                                unsigned int effective_refs,
                                                unsigned int flatten_brefs)
{
	return probe != 0 && probe->sps.valid && probe->pps.valid &&
	       probe->sps.frame_mbs_only_flag && !probe->pps.transform_8x8_mode_flag &&
	       probe->pps.weighted_pred_flag &&
	       probe->current_au.is_p && !flatten_brefs &&
	       effective_refs >= probe->sps.num_ref_frames;
}

/* Once the decoder PPS disables explicit weights, a source weight table
 * cannot be submitted unchanged. Multi-slice AUs must be inspected in full:
 * the probe describes only the first slice. Implicit bipred has no table. */
static inline int h264x_rewrite_weights_required(const struct h264x_probe *probe)
{
	if (!probe->pps.weighted_pred_flag && probe->pps.weighted_bipred_idc != 1U)
		return 0;
	if (!probe->current_au.valid || probe->current_au.parse_error)
		return 1;
	if (probe->current_au.is_idr)
		return 0;
	if (probe->current_au.vcl_count != 1U)
		return 1;
	return (probe->pps.weighted_pred_flag &&
	        (probe->current_au.is_p || probe->current_au.slice_type == 3U)) ||
	       (probe->pps.weighted_bipred_idc == 1U && probe->current_au.is_b);
}

/* Success publishes a complete AU (possibly an unchanged copy). Failure
 * leaves out_size zero; discard all output. Each slice is parsed separately.
 * Explicit list overrides are supported only for a single-slice AU. */
int h264x_rewrite_ref_mgmt_select(const struct h264x_probe *probe,
                                    const void *au_data,
                                    unsigned int au_size,
                                    void *out_data,
                                    unsigned int out_capacity,
                                    unsigned int *out_size,
                                    unsigned int rewrite_flags);

int h264x_rewrite_ref_mgmt_override(const struct h264x_probe *probe,
                                      const void *au_data,
                                      unsigned int au_size,
                                      void *out_data,
                                      unsigned int out_capacity,
                                      unsigned int *out_size,
                                      unsigned int rewrite_flags,
                                      const struct h264x_rplm_override *override_plan);

int h264x_rewrite_parameter_sets_compat(const void *sps_data,
                                        unsigned int sps_size,
                                        const void *pps_data,
                                        unsigned int pps_size,
                                        unsigned int target_num_ref_frames,
                                        unsigned int force_level_idc,
                                        unsigned int disable_weighted_prediction,
                                        void *out_sps,
                                        unsigned int out_sps_capacity,
                                        unsigned int *out_sps_size,
                                        void *out_pps,
                                        unsigned int out_pps_capacity,
                                        unsigned int *out_pps_size);

int h264x_rewrite_ref_mgmt_to_default(const struct h264x_probe *probe,
                                      const void *au_data,
                                      unsigned int au_size,
                                      void *out_data,
                                      unsigned int out_capacity,
                                      unsigned int *out_size);

#ifdef __cplusplus
}
#endif

#endif
