/*
 * h264x_dpb.h - H.264 shadow DPB / reference-state model for PMPlayer Advance.
 *
 * Milestone 3: observe the encoder-intended reference picture state.  This
 * module does not replace Sony's AVC decoder yet; it builds the state needed
 * for a real B-pyramid implementation and later ME/VME/VFPU reconstruction.
 */

#ifndef __PPA_H264X_DPB_H__
#define __PPA_H264X_DPB_H__

#include <stdint.h>

#ifndef H264X_DPB_MAX_REFS
#define H264X_DPB_MAX_REFS 16
#endif

#define H264X_DPB_RISK_MMCO_STRIPPED   0x00000001U
#define H264X_DPB_RISK_REF_B           0x00000002U
#define H264X_DPB_RISK_P_REF_MGMT      0x00000004U
#define H264X_DPB_RISK_RPLM_FOUND      0x00000008U
#define H264X_DPB_RISK_RPLM_MISSING    0x00000010U
#define H264X_DPB_RISK_TRUE_MISSING    0x00000020U
#define H264X_DPB_RISK_LARGE_AU        0x00000040U
#define H264X_DPB_RISK_DIRECT_B        0x00000080U

struct h264x_sps_info;
struct h264x_au_info;

struct h264x_dpb_ref {
	int valid;
	int short_term;
	int long_term;
	int is_reference_b;
	unsigned int frame_num;
	int frame_num_wrap;
	int poc;
	unsigned int poc_lsb;
	unsigned int long_term_frame_idx;
	unsigned int au_index;
	int timestamp;
	unsigned int nal_ref_idc;
};

struct h264x_dpb {
	int enabled;
	unsigned int max_refs;
	unsigned int max_frame_num;
	int max_long_term_frame_idx;
	struct h264x_dpb_ref refs[H264X_DPB_MAX_REFS];
	unsigned int ref_count;
	unsigned int short_count;
	unsigned int long_count;
	unsigned int intended_brefs;
	unsigned int mmco_events;
	unsigned int rplm_events;
	/* Legacy counter kept for log compatibility; now tracks true missing refs. */
	unsigned int missing_ref_events;
	unsigned int true_missing_ref_events;
	unsigned int mmco_absent_events;
	unsigned int mmco_benign_absent_events;
	unsigned int visible_risk_events;
	unsigned int risky_bref_events;
	unsigned int risky_pref_events;
	unsigned int clears;
	unsigned int last_idr_au;

	/* Per-AU classifier state, reset on every observed access unit. */
	unsigned int last_au_index;
	unsigned int last_rplm_ops;
	unsigned int last_rplm_found;
	unsigned int last_rplm_missing;
	unsigned int last_rplm_bref_refs;
	unsigned int last_mmco_ops;
	unsigned int last_mmco_found;
	unsigned int last_mmco_absent;
	unsigned int last_mmco_benign_absent;
	unsigned int last_mmco_true_missing;
	unsigned int last_visible_risk;
	unsigned int last_risk_class;
};

void h264x_dpb_init(struct h264x_dpb *dpb);
void h264x_dpb_reset(struct h264x_dpb *dpb);
void h264x_dpb_configure(struct h264x_dpb *dpb, const struct h264x_sps_info *sps);
void h264x_dpb_observe_au(struct h264x_dpb *dpb,
                          const struct h264x_sps_info *sps,
                          const struct h264x_au_info *au);

#endif
