/* H.264 parameter-set and slice-header parser. Its metadata drives the
 * compatibility rewrite, reference-state model and B-frame presentation.
 * Parsing does not modify the source access unit. */

#ifndef __PPA_H264X_PROBE_H__
#define __PPA_H264X_PROBE_H__

#include <stdint.h>
#include "h264x_dpb.h"
#include "ppa_h264x_access_unit.h"

#ifndef PPA_H264X_SLICE_HEADER_RBSP
#define PPA_H264X_SLICE_HEADER_RBSP 4096
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef H264X_MAX_RPLM_OPS
#define H264X_MAX_RPLM_OPS 8
#endif

#ifndef H264X_MAX_MMCO_OPS
#define H264X_MAX_MMCO_OPS 8
#endif

struct h264x_rplm_op {
	unsigned int modification_of_pic_nums_idc;
	unsigned int abs_diff_pic_num_minus1;
	unsigned int long_term_pic_num;
};

struct h264x_mmco_op {
	unsigned int memory_management_control_operation;
	unsigned int difference_of_pic_nums_minus1;
	unsigned int long_term_pic_num;
	unsigned int long_term_frame_idx;
	unsigned int max_long_term_frame_idx_plus1;
};

struct h264x_sps_info {
	int valid;
	unsigned int profile_idc;
	unsigned int constraint_flags;
	unsigned int level_idc;
	unsigned int seq_parameter_set_id;
	unsigned int chroma_format_idc;
	unsigned int separate_colour_plane_flag;
	unsigned int bit_depth_luma_minus8;
	unsigned int bit_depth_chroma_minus8;
	unsigned int qpprime_y_zero_transform_bypass_flag;
	unsigned int seq_scaling_matrix_present_flag;
	unsigned int log2_max_frame_num_minus4;
	unsigned int pic_order_cnt_type;
	unsigned int log2_max_pic_order_cnt_lsb_minus4;
	unsigned int delta_pic_order_always_zero_flag;
	unsigned int num_ref_frames;
	unsigned int gaps_in_frame_num_value_allowed_flag;
	unsigned int pic_width_in_mbs_minus1;
	unsigned int pic_height_in_map_units_minus1;
	unsigned int frame_mbs_only_flag;
	unsigned int mb_adaptive_frame_field_flag;
	unsigned int direct_8x8_inference_flag;
	unsigned int frame_cropping_flag;
	unsigned int frame_crop_left_offset;
	unsigned int frame_crop_right_offset;
	unsigned int frame_crop_top_offset;
	unsigned int frame_crop_bottom_offset;
	unsigned int vui_parameters_present_flag;
	unsigned int coded_width;
	unsigned int coded_height;
	unsigned int display_width;
	unsigned int display_height;
};

struct h264x_pps_info {
	int valid;
	unsigned int pic_parameter_set_id;
	unsigned int seq_parameter_set_id;
	unsigned int entropy_coding_mode_flag;
	unsigned int bottom_field_pic_order_in_frame_present_flag;
	unsigned int num_slice_groups_minus1;
	unsigned int num_ref_idx_l0_default_active_minus1;
	unsigned int num_ref_idx_l1_default_active_minus1;
	unsigned int weighted_pred_flag;
	unsigned int weighted_bipred_idc;
	int pic_init_qp_minus26;
	int pic_init_qs_minus26;
	int chroma_qp_index_offset;
	unsigned int deblocking_filter_control_present_flag;
	unsigned int constrained_intra_pred_flag;
	unsigned int redundant_pic_cnt_present_flag;
	unsigned int transform_8x8_mode_flag;
	unsigned int pic_scaling_matrix_present_flag;
	int second_chroma_qp_index_offset;
};

struct h264x_au_info {
	int valid;
	unsigned int au_index;
	int timestamp;
	unsigned int au_size;
	unsigned int nal_count;
	unsigned int vcl_count;
	unsigned int nal_unit_type;
	unsigned int nal_ref_idc;
	unsigned int slice_type_raw;
	unsigned int slice_type;
	unsigned int pic_parameter_set_id;
	unsigned int frame_num;
	unsigned int field_pic_flag;
	unsigned int bottom_field_flag;
	unsigned int idr_pic_id;
	unsigned int pic_order_cnt_lsb;
	int delta_pic_order_cnt_bottom;
	int is_idr;
	int is_i;
	int is_p;
	int is_b;
	int is_reference;
	int is_reference_b;
	int uses_8x8_transform;
	unsigned int direct_spatial_mv_pred_flag;
	unsigned int num_ref_idx_active_override_flag;
	unsigned int num_ref_idx_l0_active_minus1;
	unsigned int num_ref_idx_l1_active_minus1;
	unsigned int ref_pic_list_modification_flag_l0;
	unsigned int ref_pic_list_modification_flag_l1;
	unsigned int ref_pic_list_modification_count_l0;
	unsigned int ref_pic_list_modification_count_l1;
	unsigned int rplm_l0_count;
	unsigned int rplm_l1_count;
	struct h264x_rplm_op rplm_l0[H264X_MAX_RPLM_OPS];
	struct h264x_rplm_op rplm_l1[H264X_MAX_RPLM_OPS];
	unsigned int no_output_of_prior_pics_flag;
	unsigned int long_term_reference_flag;
	unsigned int adaptive_ref_pic_marking_mode_flag;
	unsigned int mmco_count;
	unsigned int mmco_nonzero;
	unsigned int mmco_op_count;
	struct h264x_mmco_op mmco_ops[H264X_MAX_MMCO_OPS];
	unsigned int cabac_init_idc;
	int slice_qp_delta;
	unsigned int disable_deblocking_filter_idc;
	int parse_error;
};

struct h264x_probe {
	int enabled;
	int avcc_valid;
	int parse_error;
	unsigned int source_width;
	unsigned int source_height;
	unsigned int avcc_profile_idc;
	unsigned int avcc_compatibility;
	unsigned int avcc_level_idc;
	unsigned int nal_length_size;
	unsigned int sps_count;
	unsigned int pps_count;
	unsigned int private_size;
	struct h264x_sps_info sps;
	struct h264x_pps_info pps;
	struct h264x_au_info current_au;
	struct h264x_dpb shadow_dpb;
	PpaH264xAccessUnitContext access_unit;
	unsigned int au_count;
	unsigned int reference_b_count;
	unsigned int b_slice_count;
	unsigned int p_slice_count;
	unsigned int i_slice_count;
	unsigned int transform8x8_au_count;
};

void h264x_probe_init(struct h264x_probe *p);
void h264x_probe_close(struct h264x_probe *p);
void h264x_probe_reset_stream(struct h264x_probe *p);
int  h264x_probe_open_avcc(struct h264x_probe *p,
                           const void *private_data,
                           unsigned int private_size,
                           unsigned int source_width,
                           unsigned int source_height);
int  h264x_probe_open_parameter_sets(struct h264x_probe *p,
                                      const void *sps_data,
                                      unsigned int sps_size,
                                      const void *pps_data,
                                      unsigned int pps_size,
                                      unsigned int nal_length_size,
                                      unsigned int source_width,
                                      unsigned int source_height);
void h264x_probe_before_decode(struct h264x_probe *p,
                               const void *au_data,
                               unsigned int au_size,
                               int timestamp,
                               int avc_mode);

#ifdef __cplusplus
}
#endif

#endif
