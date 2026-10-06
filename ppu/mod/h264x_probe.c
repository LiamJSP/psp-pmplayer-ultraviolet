

#include "h264x_probe.h"

#include <stdio.h>
#include <string.h>

#ifndef PPA_H264X_MAX_RBSP
#define PPA_H264X_MAX_RBSP 2048
#endif

#ifndef PPA_H264X_SLICE_HEADER_RBSP
#define PPA_H264X_SLICE_HEADER_RBSP 4096
#endif

static uint8_t g_h264x_slice_header_rbsp[PPA_H264X_SLICE_HEADER_RBSP] __attribute__((aligned(64)));

#define H264X_NAL_SLICE_NON_IDR 1
#define H264X_NAL_SLICE_DPA     2
#define H264X_NAL_SLICE_DPB     3
#define H264X_NAL_SLICE_DPC     4
#define H264X_NAL_SLICE_IDR     5
#define H264X_NAL_SEI           6
#define H264X_NAL_SPS           7
#define H264X_NAL_PPS           8

struct h264x_br {
	const uint8_t *data;
	unsigned int size;
	unsigned int bitpos;
	int error;
};

static void h264x_br_init(struct h264x_br *br, const uint8_t *data, unsigned int size)
{
	br->data = data;
	br->size = size;
	br->bitpos = 0;
	br->error = 0;
}

static unsigned int h264x_br_bits_left(const struct h264x_br *br)
{
	unsigned int total = br->size * 8U;
	if (br->bitpos >= total)
		return 0;
	return total - br->bitpos;
}

static unsigned int h264x_rbsp_bit_at(const uint8_t *data, unsigned int bitpos)
{
	return (data[bitpos >> 3] >> (7 - (bitpos & 7))) & 1U;
}

static int h264x_br_more_rbsp_data(const struct h264x_br *br)
{
	unsigned int total;
	unsigned int pos;

	if (br == 0 || br->error)
		return 0;

	total = br->size * 8U;
	pos = br->bitpos;

	if (pos >= total)
		return 0;

	if (h264x_rbsp_bit_at(br->data, pos)) {
		unsigned int i;
		for (i = pos + 1; i < total; i++) {
			if (h264x_rbsp_bit_at(br->data, i))
				return 1;
		}
		return 0;
	}

	return 1;
}

static unsigned int h264x_br_read_bit(struct h264x_br *br)
{
	unsigned int value;

	if (br->bitpos >= br->size * 8U) {
		br->error = 1;
		return 0;
	}

	value = h264x_rbsp_bit_at(br->data, br->bitpos);
	br->bitpos++;
	return value;
}

static unsigned int h264x_br_read_bits(struct h264x_br *br, unsigned int n)
{
	unsigned int value = 0;
	unsigned int i;

	if (n > 32U) {
		br->error = 1;
		return 0;
	}

	for (i = 0; i < n; i++)
		value = (value << 1) | h264x_br_read_bit(br);

	return value;
}

static unsigned int h264x_br_ue(struct h264x_br *br)
{
	unsigned int zeros = 0;
	unsigned int suffix;

	while (!br->error && h264x_br_bits_left(br) > 0 && h264x_br_read_bit(br) == 0) {
		zeros++;
		if (zeros > 31U) {
			br->error = 1;
			return 0;
		}
	}

	if (br->error)
		return 0;

	if (zeros == 0)
		return 0;

	suffix = h264x_br_read_bits(br, zeros);
	return ((1U << zeros) - 1U) + suffix;
}

static int h264x_br_se(struct h264x_br *br)
{
	unsigned int code_num = h264x_br_ue(br);
	int value = (int)((code_num + 1U) >> 1);

	if ((code_num & 1U) == 0)
		value = -value;

	return value;
}

static unsigned int h264x_rbsp_from_nal(const uint8_t *nal,
                                        unsigned int nal_size,
                                        uint8_t *rbsp,
                                        unsigned int rbsp_capacity)
{
	unsigned int i;
	unsigned int out = 0;
	unsigned int zero_count = 0;

	if (nal == 0 || rbsp == 0 || nal_size <= 1)
		return 0;

	for (i = 1; i < nal_size; i++) {
		if (zero_count >= 2U && nal[i] == 0x03U) {
			zero_count = 0U;
			continue;
		}

		if (out >= rbsp_capacity)
			return 0;

		rbsp[out++] = nal[i];
		zero_count = nal[i] == 0U ?
		             (zero_count < 2U ? zero_count + 1U : 2U) : 0U;
	}

	return out;
}

/*
 * Slice headers sit at the beginning of the VCL NAL; the residual payload can be
 * much larger than the tiny headers we need for DPB/RPLM/MMCO modelling.  The
 * original probe required the entire NAL to fit in PPA_H264X_MAX_RBSP, which made
 * ordinary larger P/IDR slices look like parse failures.  For slice-header work,
 * copy a bounded RBSP prefix and let the bitreader fail naturally only if the
 * header itself runs past that prefix.
 */
static unsigned int h264x_rbsp_prefix_from_nal(const uint8_t *nal,
                                               unsigned int nal_size,
                                               uint8_t *rbsp,
                                               unsigned int rbsp_capacity)
{
	unsigned int i;
	unsigned int out = 0;
	unsigned int zero_count = 0;

	if (nal == 0 || rbsp == 0 || nal_size <= 1 || rbsp_capacity == 0)
		return 0;

	for (i = 1; i < nal_size; i++) {
		if (zero_count >= 2U && nal[i] == 0x03U) {
			zero_count = 0U;
			continue;
		}

		if (out >= rbsp_capacity)
			break;

		rbsp[out++] = nal[i];
		zero_count = nal[i] == 0U ?
		             (zero_count < 2U ? zero_count + 1U : 2U) : 0U;
	}

	return out;
}

static void h264x_skip_scaling_list(struct h264x_br *br, unsigned int size_of_scaling_list)
{
	int last_scale = 8;
	int next_scale = 8;
	unsigned int j;

	for (j = 0; j < size_of_scaling_list && !br->error; j++) {
		if (next_scale != 0) {
			int delta_scale = h264x_br_se(br);
			next_scale = (last_scale + delta_scale + 256) & 255;
		}
		last_scale = (next_scale == 0) ? last_scale : next_scale;
	}
}

static int h264x_is_high_profile(unsigned int profile_idc)
{
	switch (profile_idc) {
	case 100: /* High */
	case 110: /* High 10 */
	case 122: /* High 4:2:2 */
	case 244: /* High 4:4:4 */
	case 44:  /* CAVLC 4:4:4 */
	case 83:
	case 86:
	case 118:
	case 128:
	case 138:
	case 139:
	case 134:
	case 135:
		return 1;
	default:
		return 0;
	}
}

static void h264x_compute_sps_dimensions(struct h264x_sps_info *sps)
{
	unsigned int width;
	unsigned int height;
	unsigned int crop_unit_x = 1;
	unsigned int crop_unit_y = 2 - sps->frame_mbs_only_flag;

	width = (sps->pic_width_in_mbs_minus1 + 1U) * 16U;
	height = (2U - sps->frame_mbs_only_flag) *
	         (sps->pic_height_in_map_units_minus1 + 1U) * 16U;

	if (sps->chroma_format_idc == 1) {
		crop_unit_x = 2;
		crop_unit_y = 2 * (2 - sps->frame_mbs_only_flag);
	}
	else if (sps->chroma_format_idc == 2) {
		crop_unit_x = 2;
		crop_unit_y = 1 * (2 - sps->frame_mbs_only_flag);
	}
	else if (sps->chroma_format_idc == 3) {
		crop_unit_x = 1;
		crop_unit_y = 1 * (2 - sps->frame_mbs_only_flag);
	}

	sps->coded_width = width;
	sps->coded_height = height;
	sps->display_width = width;
	sps->display_height = height;

	if (sps->frame_cropping_flag) {
		unsigned int crop_w = (sps->frame_crop_left_offset + sps->frame_crop_right_offset) * crop_unit_x;
		unsigned int crop_h = (sps->frame_crop_top_offset + sps->frame_crop_bottom_offset) * crop_unit_y;

		if (crop_w < sps->display_width)
			sps->display_width -= crop_w;
		if (crop_h < sps->display_height)
			sps->display_height -= crop_h;
	}
}

static int h264x_parse_sps_nal(struct h264x_sps_info *sps,
                               const uint8_t *nal,
                               unsigned int nal_size)
{
	uint8_t rbsp[PPA_H264X_MAX_RBSP];
	unsigned int rbsp_size;
	struct h264x_br br;
	unsigned int i;

	memset(sps, 0, sizeof(*sps));

	if (nal == 0 || nal_size < 4 || ((nal[0] & 0x1f) != H264X_NAL_SPS))
		return -1;

	rbsp_size = h264x_rbsp_from_nal(nal, nal_size, rbsp, sizeof(rbsp));
	if (rbsp_size == 0)
		return -1;

	h264x_br_init(&br, rbsp, rbsp_size);

	sps->profile_idc = h264x_br_read_bits(&br, 8);
	sps->constraint_flags = h264x_br_read_bits(&br, 8);
	sps->level_idc = h264x_br_read_bits(&br, 8);
	sps->seq_parameter_set_id = h264x_br_ue(&br);

	sps->chroma_format_idc = 1;
	if (h264x_is_high_profile(sps->profile_idc)) {
		sps->chroma_format_idc = h264x_br_ue(&br);
		if (sps->chroma_format_idc == 3)
			sps->separate_colour_plane_flag = h264x_br_read_bit(&br);
		sps->bit_depth_luma_minus8 = h264x_br_ue(&br);
		sps->bit_depth_chroma_minus8 = h264x_br_ue(&br);
		sps->qpprime_y_zero_transform_bypass_flag = h264x_br_read_bit(&br);
		sps->seq_scaling_matrix_present_flag = h264x_br_read_bit(&br);
		if (sps->seq_scaling_matrix_present_flag) {
			unsigned int scaling_count = (sps->chroma_format_idc != 3) ? 8 : 12;
			for (i = 0; i < scaling_count; i++) {
				unsigned int present = h264x_br_read_bit(&br);
				if (present) {
					if (i < 6)
						h264x_skip_scaling_list(&br, 16);
					else
						h264x_skip_scaling_list(&br, 64);
				}
			}
		}
	}

	sps->log2_max_frame_num_minus4 = h264x_br_ue(&br);
	sps->pic_order_cnt_type = h264x_br_ue(&br);
	if (sps->pic_order_cnt_type == 0) {
		sps->log2_max_pic_order_cnt_lsb_minus4 = h264x_br_ue(&br);
	}
	else if (sps->pic_order_cnt_type == 1) {
		unsigned int num_ref_frames_in_pic_order_cnt_cycle;
		sps->delta_pic_order_always_zero_flag = h264x_br_read_bit(&br);
		(void)h264x_br_se(&br);
		(void)h264x_br_se(&br);
		num_ref_frames_in_pic_order_cnt_cycle = h264x_br_ue(&br);
		for (i = 0; i < num_ref_frames_in_pic_order_cnt_cycle && i < 256U; i++)
			(void)h264x_br_se(&br);
	}

	sps->num_ref_frames = h264x_br_ue(&br);
	sps->gaps_in_frame_num_value_allowed_flag = h264x_br_read_bit(&br);
	sps->pic_width_in_mbs_minus1 = h264x_br_ue(&br);
	sps->pic_height_in_map_units_minus1 = h264x_br_ue(&br);
	sps->frame_mbs_only_flag = h264x_br_read_bit(&br);
	if (!sps->frame_mbs_only_flag)
		sps->mb_adaptive_frame_field_flag = h264x_br_read_bit(&br);
	sps->direct_8x8_inference_flag = h264x_br_read_bit(&br);
	sps->frame_cropping_flag = h264x_br_read_bit(&br);
	if (sps->frame_cropping_flag) {
		sps->frame_crop_left_offset = h264x_br_ue(&br);
		sps->frame_crop_right_offset = h264x_br_ue(&br);
		sps->frame_crop_top_offset = h264x_br_ue(&br);
		sps->frame_crop_bottom_offset = h264x_br_ue(&br);
	}
	sps->vui_parameters_present_flag = h264x_br_read_bit(&br);

	if (br.error)
		return -1;

	h264x_compute_sps_dimensions(sps);
	sps->valid = 1;
	return 0;
}

static int h264x_parse_pps_nal(struct h264x_pps_info *pps,
                               const uint8_t *nal,
                               unsigned int nal_size)
{
	uint8_t rbsp[PPA_H264X_MAX_RBSP];
	unsigned int rbsp_size;
	struct h264x_br br;
	unsigned int i;

	memset(pps, 0, sizeof(*pps));

	if (nal == 0 || nal_size < 2 || ((nal[0] & 0x1f) != H264X_NAL_PPS))
		return -1;

	rbsp_size = h264x_rbsp_from_nal(nal, nal_size, rbsp, sizeof(rbsp));
	if (rbsp_size == 0)
		return -1;

	h264x_br_init(&br, rbsp, rbsp_size);

	pps->pic_parameter_set_id = h264x_br_ue(&br);
	pps->seq_parameter_set_id = h264x_br_ue(&br);
	pps->entropy_coding_mode_flag = h264x_br_read_bit(&br);
	pps->bottom_field_pic_order_in_frame_present_flag = h264x_br_read_bit(&br);
	pps->num_slice_groups_minus1 = h264x_br_ue(&br);

	if (pps->num_slice_groups_minus1 > 0) {
		unsigned int slice_group_map_type = h264x_br_ue(&br);
		if (slice_group_map_type == 0) {
			for (i = 0; i <= pps->num_slice_groups_minus1; i++)
				(void)h264x_br_ue(&br);
		}
		else if (slice_group_map_type == 2) {
			for (i = 0; i < pps->num_slice_groups_minus1; i++) {
				(void)h264x_br_ue(&br);
				(void)h264x_br_ue(&br);
				(void)h264x_br_ue(&br);
			}
		}
		else if (slice_group_map_type == 3 || slice_group_map_type == 4 || slice_group_map_type == 5) {
			(void)h264x_br_read_bit(&br);
			(void)h264x_br_ue(&br);
		}
		else if (slice_group_map_type == 6) {
			unsigned int pic_size_in_map_units_minus1 = h264x_br_ue(&br);
			unsigned int bits = 0;
			unsigned int groups = pps->num_slice_groups_minus1 + 1U;
			while ((1U << bits) < groups && bits < 16U)
				bits++;
			for (i = 0; i <= pic_size_in_map_units_minus1 && i < 8192U; i++)
				(void)h264x_br_read_bits(&br, bits);
		}
	}

	pps->num_ref_idx_l0_default_active_minus1 = h264x_br_ue(&br);
	pps->num_ref_idx_l1_default_active_minus1 = h264x_br_ue(&br);
	pps->weighted_pred_flag = h264x_br_read_bit(&br);
	pps->weighted_bipred_idc = h264x_br_read_bits(&br, 2);
	pps->pic_init_qp_minus26 = h264x_br_se(&br);
	pps->pic_init_qs_minus26 = h264x_br_se(&br);
	pps->chroma_qp_index_offset = h264x_br_se(&br);
	pps->deblocking_filter_control_present_flag = h264x_br_read_bit(&br);
	pps->constrained_intra_pred_flag = h264x_br_read_bit(&br);
	pps->redundant_pic_cnt_present_flag = h264x_br_read_bit(&br);

	if (!br.error && h264x_br_more_rbsp_data(&br)) {
		pps->transform_8x8_mode_flag = h264x_br_read_bit(&br);
		pps->pic_scaling_matrix_present_flag = h264x_br_read_bit(&br);
		if (pps->pic_scaling_matrix_present_flag) {
			unsigned int scaling_count = 6 + 2 * pps->transform_8x8_mode_flag;
			for (i = 0; i < scaling_count; i++) {
				unsigned int present = h264x_br_read_bit(&br);
				if (present) {
					if (i < 6)
						h264x_skip_scaling_list(&br, 16);
					else
						h264x_skip_scaling_list(&br, 64);
				}
			}
		}
		pps->second_chroma_qp_index_offset = h264x_br_se(&br);
	}
	else {
		pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
	}

	if (br.error)
		return -1;

	pps->valid = 1;
	return 0;
}

static int h264x_read_nal_length(const uint8_t *p,
                                 unsigned int size,
                                 unsigned int nal_length_size,
                                 unsigned int *nal_size)
{
	unsigned int i;
	unsigned int value = 0;

	if (nal_length_size < 1 || nal_length_size > 4 || size < nal_length_size)
		return -1;

	for (i = 0; i < nal_length_size; i++)
		value = (value << 8) | p[i];

	*nal_size = value;
	return 0;
}

static void h264x_read_ref_pic_list_modification(struct h264x_br *br,
                                                 unsigned int list,
                                                 struct h264x_au_info *au)
{
	unsigned int modification_of_pic_nums_idc;
	unsigned int n = 0;
	unsigned int *flag;
	unsigned int *count;
	struct h264x_rplm_op *ops;

	if (list == 0) {
		flag = &au->ref_pic_list_modification_flag_l0;
		count = &au->ref_pic_list_modification_count_l0;
		ops = au->rplm_l0;
	}
	else {
		flag = &au->ref_pic_list_modification_flag_l1;
		count = &au->ref_pic_list_modification_count_l1;
		ops = au->rplm_l1;
	}

	*flag = h264x_br_read_bit(br);
	if (br->error || *flag == 0) {
		*count = 0;
		if (list == 0)
			au->rplm_l0_count = 0;
		else
			au->rplm_l1_count = 0;
		return;
	}

	do {
		struct h264x_rplm_op op;
		memset(&op, 0, sizeof(op));
		modification_of_pic_nums_idc = h264x_br_ue(br);
		op.modification_of_pic_nums_idc = modification_of_pic_nums_idc;

		if (modification_of_pic_nums_idc == 0 || modification_of_pic_nums_idc == 1)
			op.abs_diff_pic_num_minus1 = h264x_br_ue(br);
		else if (modification_of_pic_nums_idc == 2)
			op.long_term_pic_num = h264x_br_ue(br);
		else if (modification_of_pic_nums_idc != 3)
			br->error = 1;

		if (modification_of_pic_nums_idc != 3) {
			if (n < H264X_MAX_RPLM_OPS)
				ops[n] = op;
			n++;
		}
	} while (!br->error && modification_of_pic_nums_idc != 3 && n < 64U);

	if (n >= 64U)
		br->error = 1;

	*count = n;
	if (list == 0)
		au->rplm_l0_count = (n > H264X_MAX_RPLM_OPS) ? H264X_MAX_RPLM_OPS : n;
	else
		au->rplm_l1_count = (n > H264X_MAX_RPLM_OPS) ? H264X_MAX_RPLM_OPS : n;
}

static void h264x_skip_pred_weight_table(struct h264x_br *br,
                                         const struct h264x_sps_info *sps,
                                         unsigned int chroma_array_type,
                                         unsigned int l0_count,
                                         unsigned int l1_count,
                                         int has_l1)
{
	unsigned int luma_log2_weight_denom;
	unsigned int chroma_log2_weight_denom = 0;
	unsigned int i;
	unsigned int j;

	luma_log2_weight_denom = h264x_br_ue(br);
	(void)luma_log2_weight_denom;

	if (chroma_array_type != 0)
		chroma_log2_weight_denom = h264x_br_ue(br);
	(void)chroma_log2_weight_denom;

	for (i = 0; i <= l0_count && !br->error && i < 32U; i++) {
		unsigned int luma_weight_l0_flag = h264x_br_read_bit(br);
		if (luma_weight_l0_flag) {
			(void)h264x_br_se(br);
			(void)h264x_br_se(br);
		}
		if (chroma_array_type != 0) {
			unsigned int chroma_weight_l0_flag = h264x_br_read_bit(br);
			if (chroma_weight_l0_flag) {
				for (j = 0; j < 2; j++) {
					(void)h264x_br_se(br);
					(void)h264x_br_se(br);
				}
			}
		}
	}

	if (!has_l1)
		return;

	for (i = 0; i <= l1_count && !br->error && i < 32U; i++) {
		unsigned int luma_weight_l1_flag = h264x_br_read_bit(br);
		if (luma_weight_l1_flag) {
			(void)h264x_br_se(br);
			(void)h264x_br_se(br);
		}
		if (chroma_array_type != 0) {
			unsigned int chroma_weight_l1_flag = h264x_br_read_bit(br);
			if (chroma_weight_l1_flag) {
				for (j = 0; j < 2; j++) {
					(void)h264x_br_se(br);
					(void)h264x_br_se(br);
				}
			}
		}
	}

	(void)sps;
}

static void h264x_read_dec_ref_pic_marking(struct h264x_br *br,
                                           struct h264x_au_info *au)
{
	if (!au->is_reference)
		return;

	if (au->is_idr) {
		au->no_output_of_prior_pics_flag = h264x_br_read_bit(br);
		au->long_term_reference_flag = h264x_br_read_bit(br);
		return;
	}

	au->adaptive_ref_pic_marking_mode_flag = h264x_br_read_bit(br);
	if (au->adaptive_ref_pic_marking_mode_flag) {
		unsigned int mmco;
		unsigned int n = 0;
		do {
			struct h264x_mmco_op op;
			memset(&op, 0, sizeof(op));
			mmco = h264x_br_ue(br);
			op.memory_management_control_operation = mmco;
			if (mmco != 0) {
				au->mmco_nonzero = 1;
				n++;
			}
			switch (mmco) {
			case 0:
				break;
			case 1:
				op.difference_of_pic_nums_minus1 = h264x_br_ue(br);
				break;
			case 2:
				op.long_term_pic_num = h264x_br_ue(br);
				break;
			case 3:
				op.difference_of_pic_nums_minus1 = h264x_br_ue(br);
				op.long_term_frame_idx = h264x_br_ue(br);
				break;
			case 4:
				op.max_long_term_frame_idx_plus1 = h264x_br_ue(br);
				break;
			case 5:
				break;
			case 6:
				op.long_term_frame_idx = h264x_br_ue(br);
				break;
			default:
				br->error = 1;
				break;
			}
			if (mmco != 0 && au->mmco_op_count < H264X_MAX_MMCO_OPS)
				au->mmco_ops[au->mmco_op_count++] = op;
		} while (!br->error && mmco != 0 && n < 32U);

		if (n >= 32U)
			br->error = 1;
		au->mmco_count = n;
	}
}

static int h264x_parse_slice_header_rbsp(struct h264x_probe *p,
                                         const uint8_t *rbsp,
                                         unsigned int rbsp_size,
                                         struct h264x_au_info *au)
{
	struct h264x_br br;
	const struct h264x_sps_info *sps = &p->sps;
	const struct h264x_pps_info *pps = &p->pps;

	if (p == 0 || rbsp == 0 || rbsp_size == 0U || au == 0)
		return -1;

	h264x_br_init(&br, rbsp, rbsp_size);

	(void)h264x_br_ue(&br); /* first_mb_in_slice */
	au->slice_type_raw = h264x_br_ue(&br);
	au->slice_type = au->slice_type_raw % 5U;
	au->pic_parameter_set_id = h264x_br_ue(&br);

	if (sps->valid && sps->separate_colour_plane_flag)
		(void)h264x_br_read_bits(&br, 2);

	if (sps->valid) {
		unsigned int frame_num_bits = sps->log2_max_frame_num_minus4 + 4U;
		if (frame_num_bits > 16U)
			frame_num_bits = 16U;
		au->frame_num = h264x_br_read_bits(&br, frame_num_bits);

		if (!sps->frame_mbs_only_flag) {
			au->field_pic_flag = h264x_br_read_bit(&br);
			if (au->field_pic_flag)
				au->bottom_field_flag = h264x_br_read_bit(&br);
		}
	}

	if (au->nal_unit_type == H264X_NAL_SLICE_IDR)
		au->idr_pic_id = h264x_br_ue(&br);

	if (sps->valid && sps->pic_order_cnt_type == 0) {
		unsigned int poc_bits = sps->log2_max_pic_order_cnt_lsb_minus4 + 4U;
		if (poc_bits > 16U)
			poc_bits = 16U;
		au->pic_order_cnt_lsb = h264x_br_read_bits(&br, poc_bits);
		if (pps->valid && pps->bottom_field_pic_order_in_frame_present_flag && !au->field_pic_flag)
			au->delta_pic_order_cnt_bottom = h264x_br_se(&br);
	}
	else if (sps->valid && sps->pic_order_cnt_type == 1 && !sps->delta_pic_order_always_zero_flag) {
		(void)h264x_br_se(&br);
		if (pps->valid && pps->bottom_field_pic_order_in_frame_present_flag && !au->field_pic_flag)
			(void)h264x_br_se(&br);
	}

	if (pps->valid && pps->redundant_pic_cnt_present_flag)
		(void)h264x_br_ue(&br);

	au->is_i = (au->slice_type == 2 || au->slice_type == 4);
	au->is_p = (au->slice_type == 0 || au->slice_type == 3);
	au->is_b = (au->slice_type == 1);
	au->is_reference = au->nal_ref_idc != 0;
	au->is_reference_b = au->is_b && au->is_reference;
	au->is_idr = au->nal_unit_type == H264X_NAL_SLICE_IDR;
	au->uses_8x8_transform = pps->valid && pps->transform_8x8_mode_flag;

	if (au->is_b)
		au->direct_spatial_mv_pred_flag = h264x_br_read_bit(&br);

	au->num_ref_idx_l0_active_minus1 = pps->valid ?
		pps->num_ref_idx_l0_default_active_minus1 : 0;
	au->num_ref_idx_l1_active_minus1 = pps->valid ?
		pps->num_ref_idx_l1_default_active_minus1 : 0;

	if (au->is_p || au->is_b) {
		au->num_ref_idx_active_override_flag = h264x_br_read_bit(&br);
		if (au->num_ref_idx_active_override_flag) {
			au->num_ref_idx_l0_active_minus1 = h264x_br_ue(&br);
			if (au->is_b)
				au->num_ref_idx_l1_active_minus1 = h264x_br_ue(&br);
		}
	}

	if (!au->is_i) {
		h264x_read_ref_pic_list_modification(&br, 0, au);
		if (au->is_b)
			h264x_read_ref_pic_list_modification(&br, 1, au);
	}

	if (pps->valid &&
	    ((pps->weighted_pred_flag && au->is_p) ||
	     (pps->weighted_bipred_idc == 1 && au->is_b))) {
		unsigned int chroma_array_type = 0;
		if (sps->valid)
			chroma_array_type = sps->separate_colour_plane_flag ? 0 : sps->chroma_format_idc;
		h264x_skip_pred_weight_table(&br,
		                            sps,
		                            chroma_array_type,
		                            au->num_ref_idx_l0_active_minus1,
		                            au->num_ref_idx_l1_active_minus1,
		                            au->is_b);
	}

	h264x_read_dec_ref_pic_marking(&br, au);

	if (pps->valid && pps->entropy_coding_mode_flag && !au->is_i)
		au->cabac_init_idc = h264x_br_ue(&br);

	au->slice_qp_delta = h264x_br_se(&br);

	if (pps->valid && pps->deblocking_filter_control_present_flag) {
		au->disable_deblocking_filter_idc = h264x_br_ue(&br);
		if (au->disable_deblocking_filter_idc != 1) {
			(void)h264x_br_se(&br);
			(void)h264x_br_se(&br);
		}
	}

	if (br.error)
		return -1;

	return 0;
}

static int h264x_parse_slice_header(struct h264x_probe *p,
                                    const uint8_t *nal,
                                    unsigned int nal_size,
                                    struct h264x_au_info *au)
{
	uint8_t *rbsp = g_h264x_slice_header_rbsp;
	unsigned int rbsp_size;

	if (nal == 0 || nal_size < 2U)
		return -1;
	rbsp_size = h264x_rbsp_prefix_from_nal(
	    nal, nal_size, rbsp, PPA_H264X_SLICE_HEADER_RBSP);
	if (rbsp_size == 0U)
		return -1;
	return h264x_parse_slice_header_rbsp(p, rbsp, rbsp_size, au);
}

void h264x_probe_init(struct h264x_probe *p)
{
	if (p == 0)
		return;

	memset(p, 0, sizeof(*p));
	p->enabled = 1;
	h264x_dpb_init(&p->shadow_dpb);
	ppa_h264x_access_unit_init(&p->access_unit);
}

void h264x_probe_close(struct h264x_probe *p)
{
	if (p == 0)
		return;
	ppa_h264x_access_unit_close(&p->access_unit);
}

void h264x_probe_reset_stream(struct h264x_probe *p)
{
	if (p == 0)
		return;

	memset(&p->current_au, 0, sizeof(p->current_au));
	p->au_count = 0;
	p->reference_b_count = 0;
	p->b_slice_count = 0;
	p->p_slice_count = 0;
	p->i_slice_count = 0;
	p->transform8x8_au_count = 0;
	h264x_dpb_reset(&p->shadow_dpb);
	h264x_dpb_configure(&p->shadow_dpb, &p->sps);
	ppa_h264x_access_unit_reset_stream(&p->access_unit);
}

int h264x_probe_open_avcc(struct h264x_probe *p,
                          const void *private_data,
                          unsigned int private_size,
                          unsigned int source_width,
                          unsigned int source_height)
{
	const uint8_t *avcc = (const uint8_t *)private_data;
	unsigned int pos;
	unsigned int i;
	int first_sps_done = 0;
	int first_pps_done = 0;

	if (p == 0)
		return -1;

	h264x_probe_init(p);
	p->source_width = source_width;
	p->source_height = source_height;
	p->private_size = private_size;

	if (avcc == 0 || private_size < 7) {
		p->parse_error = 1;
		return -1;
	}

	p->avcc_profile_idc = avcc[1];
	p->avcc_compatibility = avcc[2];
	p->avcc_level_idc = avcc[3];
	p->nal_length_size = (avcc[4] & 0x03U) + 1U;
	p->sps_count = avcc[5] & 0x1fU;
	pos = 6;

	for (i = 0; i < p->sps_count; i++) {
		unsigned int nal_size;
		if (pos + 2 > private_size) {
			p->parse_error = 1;
			break;
		}
		nal_size = ((unsigned int)avcc[pos] << 8) | avcc[pos + 1];
		pos += 2;
		if (pos + nal_size > private_size) {
			p->parse_error = 1;
			break;
		}
		if (!first_sps_done) {
			if (h264x_parse_sps_nal(&p->sps, avcc + pos, nal_size) != 0)
				p->parse_error = 1;
			first_sps_done = 1;
		}
		pos += nal_size;
	}

	if (!p->parse_error && pos < private_size) {
		p->pps_count = avcc[pos++];
		for (i = 0; i < p->pps_count; i++) {
			unsigned int nal_size;
			if (pos + 2 > private_size) {
				p->parse_error = 1;
				break;
			}
			nal_size = ((unsigned int)avcc[pos] << 8) | avcc[pos + 1];
			pos += 2;
			if (pos + nal_size > private_size) {
				p->parse_error = 1;
				break;
			}
			if (!first_pps_done) {
				if (h264x_parse_pps_nal(&p->pps, avcc + pos, nal_size) != 0)
					p->parse_error = 1;
				first_pps_done = 1;
			}
			pos += nal_size;
		}
	}

	p->avcc_valid = !p->parse_error && p->sps.valid && p->pps.valid;
	h264x_dpb_configure(&p->shadow_dpb, &p->sps);
	return p->avcc_valid ? 0 : -1;
}

int h264x_probe_open_parameter_sets(struct h264x_probe *p,
                                     const void *sps_data,
                                     unsigned int sps_size,
                                     const void *pps_data,
                                     unsigned int pps_size,
                                     unsigned int nal_length_size,
                                     unsigned int source_width,
                                     unsigned int source_height)
{
	const uint8_t *sps = (const uint8_t *)sps_data;
	const uint8_t *pps = (const uint8_t *)pps_data;

	if (p == 0)
		return -1;

	h264x_probe_init(p);
	p->source_width = source_width;
	p->source_height = source_height;
	p->private_size = sps_size + pps_size;
	p->nal_length_size = nal_length_size;
	p->sps_count = sps_size != 0U ? 1U : 0U;
	p->pps_count = pps_size != 0U ? 1U : 0U;

	if (sps == 0 || pps == 0 || sps_size < 4U || pps_size < 2U ||
	    nal_length_size < 1U || nal_length_size > 4U) {
		p->parse_error = 1;
		return -1;
	}

	if (h264x_parse_sps_nal(&p->sps, sps, sps_size) != 0 ||
	    h264x_parse_pps_nal(&p->pps, pps, pps_size) != 0)
		p->parse_error = 1;

	if (sps_size > 3U) {
		p->avcc_profile_idc = sps[1];
		p->avcc_compatibility = sps[2];
		p->avcc_level_idc = sps[3];
	}
	p->avcc_valid = !p->parse_error && p->sps.valid && p->pps.valid;
	h264x_dpb_configure(&p->shadow_dpb, &p->sps);
	return p->avcc_valid ? 0 : -1;
}

void h264x_probe_before_decode(struct h264x_probe *p,
                               const void *au_data,
                               unsigned int au_size,
                               int timestamp,
                               int avc_mode)
{
	const uint8_t *data = (const uint8_t *)au_data;
	unsigned int pos = 0;
	struct h264x_au_info *au;
	int prepared_result;

	if (p == 0 || !p->enabled)
		return;

	au = &p->current_au;
	memset(au, 0, sizeof(*au));

	p->au_count++;
	au->au_index = p->au_count;
	au->timestamp = timestamp;
	au->au_size = au_size;

	if (data == 0 || au_size == 0 || p->nal_length_size == 0) {
		au->parse_error = 1;
		return;
	}

	prepared_result = ppa_h264x_access_unit_prepare(
	    &p->access_unit, data, au_size, p->nal_length_size);
	if (prepared_result == PPA_H264X_AU_OK) {
		unsigned int i;
		for (i = 0U; i < p->access_unit.record_count; ++i) {
			const PpaH264xNalRecord *record =
			    ppa_h264x_access_unit_record(&p->access_unit, i);
			unsigned int nal_type;
			unsigned int nal_ref_idc;

			if (record == 0) {
				au->parse_error = 1;
				break;
			}
			nal_type = record->nal_unit_type;
			nal_ref_idc = record->nal_ref_idc;
			au->nal_count++;
			if (nal_type == H264X_NAL_SLICE_NON_IDR ||
			    nal_type == H264X_NAL_SLICE_IDR) {
				const uint8_t *rbsp;
				au->vcl_count++;
				if (au->valid)
					continue;
				au->nal_unit_type = nal_type;
				au->nal_ref_idc = nal_ref_idc;
				rbsp = ppa_h264x_access_unit_rbsp(
				    &p->access_unit, record);
				if (rbsp != 0 && record->rbsp_size != 0U &&
				    h264x_parse_slice_header_rbsp(
				        p, rbsp, record->rbsp_size, au) == 0)
					au->valid = 1;
				else
					au->parse_error = 1;
			}
		}
	}
	else {
		while (pos <= au_size &&
		       p->nal_length_size <= au_size - pos) {
			unsigned int nal_size = 0;
			unsigned int nal_type;
			unsigned int nal_ref_idc;

			if (h264x_read_nal_length(data + pos, au_size - pos, p->nal_length_size, &nal_size) != 0) {
				au->parse_error = 1;
				break;
			}
			pos += p->nal_length_size;

			if (nal_size == 0) {
				continue;
			}

			if (nal_size > au_size - pos) {
				au->parse_error = 1;
				break;
			}

			nal_type = data[pos] & 0x1fU;
			nal_ref_idc = (data[pos] >> 5) & 0x03U;
			au->nal_count++;

			if (nal_type == H264X_NAL_SLICE_NON_IDR || nal_type == H264X_NAL_SLICE_IDR) {
				au->vcl_count++;
				if (!au->valid) {
					au->nal_unit_type = nal_type;
					au->nal_ref_idc = nal_ref_idc;
					if (h264x_parse_slice_header(p, data + pos, nal_size, au) == 0)
						au->valid = 1;
					else
						au->parse_error = 1;
				}
			}

			pos += nal_size;
		}
	}

	if (au->is_i)
		p->i_slice_count++;
	else if (au->is_p)
		p->p_slice_count++;
	else if (au->is_b)
		p->b_slice_count++;

	if (au->is_reference_b)
		p->reference_b_count++;
	if (au->uses_8x8_transform)
		p->transform8x8_au_count++;

	h264x_dpb_observe_au(&p->shadow_dpb, &p->sps, au);

}

