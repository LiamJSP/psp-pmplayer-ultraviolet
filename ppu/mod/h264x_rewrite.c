/*
 * h264x_rewrite.c - AVC compatibility rewrite helpers for PPA.
 *
 * This bridge deliberately targets a narrow compatibility envelope:
 *   - length-prefixed AVC access units from MKV/MP4 avcC;
 *   - progressive non-IDR slice headers using one SPS/PPS and slice group;
 *   - reference-list/marking simplification and explicit weight removal.
 *
 * It does not claim bitstream-general transcoding.  It is a controlled probe
 * for the observed PSP failure mode where a Main-profile B-pyramid stream
 * decodes a reference B-frame, then fails on the following P-frame with RPLM
 * and MMCO syntax.
 */

#include "h264x_rewrite.h"

#include <stdio.h>
#include <string.h>

#if PPA_H264X_REWRITE_ENABLED

#ifndef PPA_H264X_REWRITE_MAX_RBSP
#define PPA_H264X_REWRITE_MAX_RBSP (256U * 1024U)
#endif

#define H264X_NAL_SLICE_NON_IDR 1
#define H264X_NAL_SLICE_IDR     5

static uint8_t g_h264x_rewrite_rbsp[PPA_H264X_REWRITE_MAX_RBSP] __attribute__((aligned(64)));
static uint8_t g_h264x_rewrite_new_rbsp[PPA_H264X_REWRITE_MAX_RBSP] __attribute__((aligned(64)));

struct h264x_rbr {
	const uint8_t *data;
	unsigned int size;
	unsigned int bitpos;
	int error;
};

struct h264x_bw {
	uint8_t *data;
	unsigned int capacity;
	unsigned int bitpos;
	int error;
};

struct h264x_rewrite_splice {
	unsigned int slice_type;
	unsigned int nal_ref_idc;
	unsigned int nal_unit_type;
	unsigned int rplm_start_bit;
	unsigned int rplm_end_bit;
	unsigned int rplm_replacement_bits;
	unsigned int weight_start_bit;
	unsigned int weight_end_bit;
	unsigned int marking_start_bit;
	unsigned int marking_end_bit;
	unsigned int marking_replacement_bits;
	unsigned int header_end_bit;
	unsigned int cabac_payload_start_bit;
	unsigned int is_cabac;
	unsigned int changed_rplm;
	unsigned int changed_weight;
	unsigned int changed_marking;
};

static void h264x_rbr_init(struct h264x_rbr *br, const uint8_t *data, unsigned int size)
{
	br->data = data;
	br->size = size;
	br->bitpos = 0;
	br->error = 0;
}

static unsigned int h264x_rbr_bits_left(const struct h264x_rbr *br)
{
	unsigned int total;

	total = br->size * 8U;
	if (br->bitpos >= total)
		return 0;
	return total - br->bitpos;
}

static unsigned int h264x_rbr_bit_at(const uint8_t *data, unsigned int bitpos)
{
	return (data[bitpos >> 3] >> (7 - (bitpos & 7))) & 1U;
}

static unsigned int h264x_rbr_read_bit(struct h264x_rbr *br)
{
	unsigned int v;

	if (br->bitpos >= br->size * 8U) {
		br->error = 1;
		return 0;
	}

	v = h264x_rbr_bit_at(br->data, br->bitpos);
	br->bitpos++;
	return v;
}

static unsigned int h264x_rbr_read_bits(struct h264x_rbr *br, unsigned int n)
{
	unsigned int v;
	unsigned int i;

	if (n > 32U) {
		br->error = 1;
		return 0;
	}

	v = 0;
	for (i = 0; i < n; i++)
		v = (v << 1) | h264x_rbr_read_bit(br);
	return v;
}

static unsigned int h264x_rbr_ue(struct h264x_rbr *br)
{
	unsigned int zeros;
	unsigned int suffix;

	zeros = 0;
	for (;;) {
		if (br->error || h264x_rbr_bits_left(br) == 0U) {
			br->error = 1;
			return 0;
		}
		if (h264x_rbr_read_bit(br) != 0U)
			break;
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

	suffix = h264x_rbr_read_bits(br, zeros);
	return ((1U << zeros) - 1U) + suffix;
}

static int h264x_rbr_se(struct h264x_rbr *br)
{
	unsigned int code_num;
	int v;

	code_num = h264x_rbr_ue(br);
	v = (int)((code_num + 1U) >> 1);
	if ((code_num & 1U) == 0)
		v = -v;
	return v;
}

static void h264x_bw_init(struct h264x_bw *bw, uint8_t *data, unsigned int capacity)
{
	bw->data = data;
	bw->capacity = capacity;
	bw->bitpos = 0;
	bw->error = 0;
	/* Bytes are initialized when first written, avoiding a fixed 256 KiB
	 * clear for every rewritten slice or parameter set. */
}

static void h264x_bw_put_bit(struct h264x_bw *bw, unsigned int bit)
{
	unsigned int byte_pos;

	byte_pos = bw->bitpos >> 3;
	if (byte_pos >= bw->capacity) {
		bw->error = 1;
		return;
	}

	if ((bw->bitpos & 7U) == 0U)
		bw->data[byte_pos] = 0U;
	if (bit & 1U)
		bw->data[byte_pos] |= (uint8_t)(0x80U >> (bw->bitpos & 7));
	bw->bitpos++;
}

static void h264x_bw_put_bits(struct h264x_bw *bw,
                              unsigned int value,
                              unsigned int count)
{
	unsigned int i;
	if (count > 32U) {
		bw->error = 1;
		return;
	}
	for (i = 0; i < count; i++) {
		unsigned int shift = count - 1U - i;
		h264x_bw_put_bit(bw, (value >> shift) & 1U);
	}
}

static void h264x_bw_put_ue(struct h264x_bw *bw, unsigned int value)
{
	unsigned int code_num;
	unsigned int bits;
	unsigned int i;
	if (value == 0xffffffffU) {
		bw->error = 1;
		return;
	}
	code_num = value + 1U;
	bits = 0U;
	for (i = code_num; i != 0U; i >>= 1U)
		bits++;
	for (i = 1U; i < bits; i++)
		h264x_bw_put_bit(bw, 0U);
	h264x_bw_put_bits(bw, code_num, bits);
}

static void h264x_bw_copy_bits(struct h264x_bw *bw,
                               const uint8_t *src,
                               unsigned int start_bit,
                               unsigned int end_bit)
{
	unsigned int count;
	unsigned int bytes;
	unsigned int shift;
	unsigned int i;

	if (bw->error || end_bit < start_bit ||
	    bw->bitpos > bw->capacity * 8U ||
	    end_bit - start_bit > bw->capacity * 8U - bw->bitpos) {
		bw->error = 1;
		return;
	}
	while (start_bit < end_bit && (bw->bitpos & 7U) != 0U)
		h264x_bw_put_bit(bw, h264x_rbr_bit_at(src, start_bit++));
	count = end_bit - start_bit;
	bytes = count >> 3;
	shift = start_bit & 7U;
	if (bytes != 0U) {
		uint8_t *dst = bw->data + (bw->bitpos >> 3);
		const uint8_t *in = src + (start_bit >> 3);
		if (shift == 0U)
			memcpy(dst, in, bytes);
		else {
			/* Eight requested bits at a non-byte boundary guarantee the
			 * lookahead byte exists. Keep compressed data byte-oriented. */
			for (i = 0U; i < bytes; ++i)
				dst[i] = (uint8_t)((in[i] << shift) | (in[i + 1U] >> (8U - shift)));
		}
		bw->bitpos += bytes * 8U;
		start_bit += bytes * 8U;
	}
	while (start_bit < end_bit)
		h264x_bw_put_bit(bw, h264x_rbr_bit_at(src, start_bit++));
}

/* Include rbsp_stop_one_bit, but not its old byte-alignment zero bits.
 * CAVLC and parameter-set splices can change the final byte alignment. */
static unsigned int h264x_rbsp_end_bit(const uint8_t *rbsp, unsigned int size)
{
	unsigned int end = size * 8U;
	while (end != 0U && h264x_rbr_bit_at(rbsp, end - 1U) == 0U)
		--end;
	return end;
}

static unsigned int h264x_read_nal_length(const uint8_t *p, unsigned int nal_length_size)
{
	unsigned int v;
	unsigned int i;

	v = 0;
	for (i = 0; i < nal_length_size; i++)
		v = (v << 8) | p[i];
	return v;
}

static void h264x_write_nal_length(uint8_t *p,
                                   unsigned int nal_length_size,
                                   unsigned int nal_size)
{
	unsigned int i;

	for (i = 0; i < nal_length_size; i++) {
		unsigned int shift;
		shift = (nal_length_size - 1U - i) * 8U;
		p[i] = (uint8_t)((nal_size >> shift) & 0xFFU);
	}
}

static unsigned int h264x_payload_to_rbsp(const uint8_t *payload,
                                          unsigned int payload_size,
                                          uint8_t *rbsp,
                                          unsigned int rbsp_capacity)
{
	unsigned int i;
	unsigned int out;
	unsigned int zeros;

	out = 0;
	zeros = 0;
	for (i = 0; i < payload_size; i++) {
		uint8_t b;

		b = payload[i];
		if (zeros == 2 && b == 0x03) {
			zeros = 0;
			continue;
		}

		if (out >= rbsp_capacity)
			return 0;
		rbsp[out++] = b;

		if (b == 0)
			zeros++;
		else
			zeros = 0;
	}

	return out;
}

static unsigned int h264x_rbsp_to_payload(const uint8_t *rbsp,
                                          unsigned int rbsp_size,
                                          uint8_t *payload,
                                          unsigned int payload_capacity)
{
	unsigned int i;
	unsigned int out;
	unsigned int zeros;

	out = 0;
	zeros = 0;
	for (i = 0; i < rbsp_size; i++) {
		uint8_t b;

		b = rbsp[i];
		if (zeros >= 2 && b <= 0x03) {
			if (out >= payload_capacity)
				return 0;
			payload[out++] = 0x03;
			zeros = 0;
		}

		if (out >= payload_capacity)
			return 0;
		payload[out++] = b;

		if (b == 0)
			zeros++;
		else
			zeros = 0;
	}

	return out;
}

static void h264x_skip_scaling_list(struct h264x_rbr *br, unsigned int size)
{
	int last_scale;
	int next_scale;
	unsigned int j;
	last_scale = 8;
	next_scale = 8;
	for (j = 0; j < size && !br->error; j++) {
		if (next_scale != 0) {
			int delta_scale = h264x_rbr_se(br);
			next_scale = (last_scale + delta_scale + 256) & 255;
		}
		last_scale = next_scale == 0 ? last_scale : next_scale;
	}
}

static int h264x_find_sps_refs(const uint8_t *rbsp,
                               unsigned int rbsp_size,
                               unsigned int *level_start,
                               unsigned int *refs_start,
                               unsigned int *refs_end)
{
	struct h264x_rbr br;
	unsigned int profile_idc;
	unsigned int chroma_format_idc;
	unsigned int seq_scaling_matrix_present_flag;
	unsigned int i;
	unsigned int pic_order_cnt_type;

	if (level_start) *level_start = 16U;
	if (refs_start) *refs_start = 0U;
	if (refs_end) *refs_end = 0U;
	h264x_rbr_init(&br, rbsp, rbsp_size);
	profile_idc = h264x_rbr_read_bits(&br, 8);
	(void)h264x_rbr_read_bits(&br, 8);
	(void)h264x_rbr_read_bits(&br, 8);
	(void)h264x_rbr_ue(&br);
	if (profile_idc == 100U || profile_idc == 110U ||
	    profile_idc == 122U || profile_idc == 244U ||
	    profile_idc == 44U || profile_idc == 83U ||
	    profile_idc == 86U || profile_idc == 118U ||
	    profile_idc == 128U || profile_idc == 138U ||
	    profile_idc == 139U || profile_idc == 134U ||
	    profile_idc == 135U) {
		chroma_format_idc = h264x_rbr_ue(&br);
		if (chroma_format_idc == 3U)
			(void)h264x_rbr_read_bit(&br);
		(void)h264x_rbr_ue(&br);
		(void)h264x_rbr_ue(&br);
		(void)h264x_rbr_read_bit(&br);
		seq_scaling_matrix_present_flag = h264x_rbr_read_bit(&br);
		if (seq_scaling_matrix_present_flag) {
			unsigned int list_count = chroma_format_idc != 3U ? 8U : 12U;
			for (i = 0; i < list_count && !br.error; i++) {
				unsigned int present = h264x_rbr_read_bit(&br);
				if (present)
					h264x_skip_scaling_list(&br, i < 6U ? 16U : 64U);
			}
		}
	}
	(void)h264x_rbr_ue(&br);
	pic_order_cnt_type = h264x_rbr_ue(&br);
	if (pic_order_cnt_type == 0U) {
		(void)h264x_rbr_ue(&br);
	} else if (pic_order_cnt_type == 1U) {
		unsigned int cycle;
		(void)h264x_rbr_read_bit(&br);
		(void)h264x_rbr_se(&br);
		(void)h264x_rbr_se(&br);
		cycle = h264x_rbr_ue(&br);
		if (cycle > 255U)
			return 0;
		for (i = 0; i < cycle && i < 256U && !br.error; i++)
			(void)h264x_rbr_se(&br);
	} else if (pic_order_cnt_type != 2U)
		return 0;
	if (br.error)
		return 0;
	if (refs_start) *refs_start = br.bitpos;
	(void)h264x_rbr_ue(&br);
	if (br.error)
		return 0;
	if (refs_end) *refs_end = br.bitpos;
	return 1;
}

static int h264x_skip_slice_groups(struct h264x_rbr *br,
                                   unsigned int num_slice_groups_minus1)
{
	unsigned int map_type;
	unsigned int i;
	if (num_slice_groups_minus1 == 0U)
		return 1;
	if (num_slice_groups_minus1 > 7U) {
		br->error = 1;
		return 0;
	}
	map_type = h264x_rbr_ue(br);
	if (map_type == 0U) {
		for (i = 0; i <= num_slice_groups_minus1 && !br->error; i++)
			(void)h264x_rbr_ue(br);
	} else if (map_type == 2U) {
		for (i = 0; i < num_slice_groups_minus1 && !br->error; i++) {
			(void)h264x_rbr_ue(br);
			(void)h264x_rbr_ue(br);
		}
	} else if (map_type == 3U || map_type == 4U || map_type == 5U) {
		(void)h264x_rbr_read_bit(br);
		(void)h264x_rbr_ue(br);
	} else if (map_type == 6U) {
		unsigned int pic_size = h264x_rbr_ue(br);
		unsigned int groups = num_slice_groups_minus1 + 1U;
		unsigned int bits = 0U;
		unsigned int v = groups - 1U;
		while (v != 0U) {
			bits++;
			v >>= 1U;
		}
		if (bits == 0U)
			bits = 1U;
		for (i = 0; i <= pic_size && !br->error; i++)
			(void)h264x_rbr_read_bits(br, bits);
	} else {
		br->error = 1;
	}
	return br->error ? 0 : 1;
}

static int h264x_find_pps_weight_bits(const uint8_t *rbsp,
                                      unsigned int rbsp_size,
                                      unsigned int *weight_start,
                                      unsigned int *weight_end)
{
	struct h264x_rbr br;
	unsigned int groups;
	h264x_rbr_init(&br, rbsp, rbsp_size);
	(void)h264x_rbr_ue(&br);
	(void)h264x_rbr_ue(&br);
	(void)h264x_rbr_read_bit(&br);
	(void)h264x_rbr_read_bit(&br);
	groups = h264x_rbr_ue(&br);
	if (!h264x_skip_slice_groups(&br, groups))
		return 0;
	(void)h264x_rbr_ue(&br);
	(void)h264x_rbr_ue(&br);
	if (br.error)
		return 0;
	if (weight_start) *weight_start = br.bitpos;
	(void)h264x_rbr_read_bit(&br);
	(void)h264x_rbr_read_bits(&br, 2);
	if (br.error)
		return 0;
	if (weight_end) *weight_end = br.bitpos;
	return 1;
}

static int h264x_rewrite_one_parameter_set(const uint8_t *nal,
                                           unsigned int nal_size,
                                           uint8_t *out,
                                           unsigned int out_capacity,
                                           unsigned int target_refs,
                                           unsigned int force_level_idc,
                                           unsigned int disable_weights,
                                           int is_sps)
{
	uint8_t *rbsp = g_h264x_rewrite_rbsp;
	uint8_t *new_rbsp = g_h264x_rewrite_new_rbsp;
	unsigned int rbsp_size;
	unsigned int payload_size;
	struct h264x_bw bw;
	unsigned int a;
	unsigned int b;
	unsigned int end_bit;
	if (nal == 0 || out == 0 || nal_size < 2U || out_capacity < 2U)
		return 0;
	rbsp_size = h264x_payload_to_rbsp(nal + 1, nal_size - 1U,
	                                 rbsp, PPA_H264X_REWRITE_MAX_RBSP);
	if (rbsp_size == 0U)
		return 0;
	end_bit = h264x_rbsp_end_bit(rbsp, rbsp_size);
	if (end_bit == 0U)
		return 0;
	h264x_bw_init(&bw, new_rbsp, PPA_H264X_REWRITE_MAX_RBSP);
	if (is_sps) {
		unsigned int level_start;
		if (!h264x_find_sps_refs(rbsp, rbsp_size, &level_start, &a, &b))
			return 0;
		if (b >= end_bit)
			return 0;
		if (force_level_idc != 0U) {
			h264x_bw_copy_bits(&bw, rbsp, 0U, level_start);
			h264x_bw_put_bits(&bw, force_level_idc & 0xFFU, 8U);
			h264x_bw_copy_bits(&bw, rbsp, level_start + 8U, a);
		} else {
			h264x_bw_copy_bits(&bw, rbsp, 0U, a);
		}
		h264x_bw_put_ue(&bw, target_refs);
		h264x_bw_copy_bits(&bw, rbsp, b, end_bit);
	} else {
		if (!h264x_find_pps_weight_bits(rbsp, rbsp_size, &a, &b))
			return 0;
		if (b >= end_bit)
			return 0;
		h264x_bw_copy_bits(&bw, rbsp, 0U, a);
		if (disable_weights) {
			h264x_bw_put_bit(&bw, 0U);
			h264x_bw_put_bits(&bw, 0U, 2U);
		} else {
			h264x_bw_copy_bits(&bw, rbsp, a, b);
		}
		h264x_bw_copy_bits(&bw, rbsp, b, end_bit);
	}
	if (bw.error)
		return 0;
	payload_size = h264x_rbsp_to_payload(new_rbsp,
	                                    (bw.bitpos + 7U) >> 3,
	                                    out + 1U,
	                                    out_capacity - 1U);
	if (payload_size == 0U)
		return 0;
	out[0] = nal[0];
	return (int)(payload_size + 1U);
}

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
                                        unsigned int *out_pps_size)
{
	int a;
	int b;
	if (out_sps_size) *out_sps_size = 0U;
	if (out_pps_size) *out_pps_size = 0U;
	a = h264x_rewrite_one_parameter_set((const uint8_t *)sps_data, sps_size,
	                                    (uint8_t *)out_sps, out_sps_capacity,
	                                    target_num_ref_frames,
	                                    force_level_idc,
	                                    0U, 1);
	b = h264x_rewrite_one_parameter_set((const uint8_t *)pps_data, pps_size,
	                                    (uint8_t *)out_pps, out_pps_capacity,
	                                    0U, 0U,
	                                    disable_weighted_prediction, 0);
	if (a <= 0 || b <= 0)
		return 0;
	if (out_sps_size) *out_sps_size = (unsigned int)a;
	if (out_pps_size) *out_pps_size = (unsigned int)b;
	return 1;
}

static void h264x_skip_ref_pic_list_modification(struct h264x_rbr *br,
                                                 unsigned int slice_type)
{
	unsigned int flag_l0;
	unsigned int flag_l1;
	unsigned int modification_of_pic_nums_idc;
	unsigned int n;

	flag_l0 = h264x_rbr_read_bit(br);
	if (flag_l0) {
		n = 0;
		do {
			modification_of_pic_nums_idc = h264x_rbr_ue(br);
			if (modification_of_pic_nums_idc == 0 || modification_of_pic_nums_idc == 1)
				(void)h264x_rbr_ue(br);
			else if (modification_of_pic_nums_idc == 2)
				(void)h264x_rbr_ue(br);
			else if (modification_of_pic_nums_idc != 3)
				br->error = 1;
			n++;
		} while (!br->error && modification_of_pic_nums_idc != 3 && n < 64U);
		if (modification_of_pic_nums_idc != 3U)
			br->error = 1;
	}

	if (slice_type == 1) {
		flag_l1 = h264x_rbr_read_bit(br);
		if (flag_l1) {
			n = 0;
			do {
				modification_of_pic_nums_idc = h264x_rbr_ue(br);
				if (modification_of_pic_nums_idc == 0 || modification_of_pic_nums_idc == 1)
					(void)h264x_rbr_ue(br);
				else if (modification_of_pic_nums_idc == 2)
					(void)h264x_rbr_ue(br);
				else if (modification_of_pic_nums_idc != 3)
					br->error = 1;
				n++;
			} while (!br->error && modification_of_pic_nums_idc != 3 && n < 64U);
			if (modification_of_pic_nums_idc != 3U)
				br->error = 1;
		}
	}
}

static void h264x_skip_pred_weight_table(struct h264x_rbr *br,
                                         const struct h264x_sps_info *sps,
                                         unsigned int chroma_array_type,
                                         unsigned int l0_count,
                                         unsigned int l1_count,
                                         int has_l1)
{
	unsigned int i;
	unsigned int j;
	unsigned int luma_weight_l0_flag;
	unsigned int chroma_weight_l0_flag;
	unsigned int luma_weight_l1_flag;
	unsigned int chroma_weight_l1_flag;
	if (l0_count >= 32U || (has_l1 && l1_count >= 32U)) {
		br->error = 1;
		return;
	}

	(void)h264x_rbr_ue(br);
	if (chroma_array_type != 0)
		(void)h264x_rbr_ue(br);

	for (i = 0; i <= l0_count && !br->error && i < 32U; i++) {
		luma_weight_l0_flag = h264x_rbr_read_bit(br);
		if (luma_weight_l0_flag) {
			(void)h264x_rbr_se(br);
			(void)h264x_rbr_se(br);
		}
		if (chroma_array_type != 0) {
			chroma_weight_l0_flag = h264x_rbr_read_bit(br);
			if (chroma_weight_l0_flag) {
				for (j = 0; j < 2; j++) {
					(void)h264x_rbr_se(br);
					(void)h264x_rbr_se(br);
				}
			}
		}
	}

	if (!has_l1)
		return;

	for (i = 0; i <= l1_count && !br->error && i < 32U; i++) {
		luma_weight_l1_flag = h264x_rbr_read_bit(br);
		if (luma_weight_l1_flag) {
			(void)h264x_rbr_se(br);
			(void)h264x_rbr_se(br);
		}
		if (chroma_array_type != 0) {
			chroma_weight_l1_flag = h264x_rbr_read_bit(br);
			if (chroma_weight_l1_flag) {
				for (j = 0; j < 2; j++) {
					(void)h264x_rbr_se(br);
					(void)h264x_rbr_se(br);
				}
			}
		}
	}

	(void)sps;
}

static void h264x_skip_dec_ref_pic_marking(struct h264x_rbr *br,
                                           unsigned int nal_unit_type)
{
	unsigned int adaptive_ref_pic_marking_mode_flag;
	unsigned int mmco;
	unsigned int n;

	if (nal_unit_type == H264X_NAL_SLICE_IDR) {
		(void)h264x_rbr_read_bit(br);
		(void)h264x_rbr_read_bit(br);
		return;
	}

	adaptive_ref_pic_marking_mode_flag = h264x_rbr_read_bit(br);
	if (!adaptive_ref_pic_marking_mode_flag)
		return;

	n = 0;
	do {
		mmco = h264x_rbr_ue(br);
		if (mmco == 1 || mmco == 3)
			(void)h264x_rbr_ue(br);
		if (mmco == 2)
			(void)h264x_rbr_ue(br);
		if (mmco == 3 || mmco == 6)
			(void)h264x_rbr_ue(br);
		if (mmco == 4)
			(void)h264x_rbr_ue(br);
		if (mmco > 6)
			br->error = 1;
		n++;
	} while (!br->error && mmco != 0 && n < 64U);
	if (mmco != 0U)
		br->error = 1;
}

static int h264x_find_splice_points(const struct h264x_probe *probe,
                                    const uint8_t *rbsp,
                                    unsigned int rbsp_size,
                                    uint8_t nal_header,
                                    struct h264x_rewrite_splice *sp)
{
	struct h264x_rbr br;
	unsigned int slice_type_raw;
	unsigned int slice_type;
	unsigned int chroma_array_type;
	unsigned int l0_count;
	unsigned int l1_count;
	int has_pred_weight;

	memset(sp, 0, sizeof(*sp));
	sp->nal_ref_idc = (nal_header >> 5) & 3U;
	sp->nal_unit_type = nal_header & 0x1FU;
	if (probe == 0 || !probe->sps.valid || !probe->pps.valid)
		return 0;
	/* This bridge models progressive frame pictures, one PPS/SPS pair and
	 * one slice group. Do not splice using guessed field or parameter-set syntax. */
	if (!probe->sps.frame_mbs_only_flag || probe->sps.separate_colour_plane_flag ||
	    probe->pps.num_slice_groups_minus1 != 0U ||
	    probe->pps.seq_parameter_set_id != probe->sps.seq_parameter_set_id ||
	    probe->sps.log2_max_frame_num_minus4 > 12U ||
	    probe->sps.pic_order_cnt_type > 2U ||
	    (probe->sps.pic_order_cnt_type == 0U &&
	     probe->sps.log2_max_pic_order_cnt_lsb_minus4 > 12U))
		return 0;
	if (sp->nal_unit_type != H264X_NAL_SLICE_NON_IDR)
		return 0;

	h264x_rbr_init(&br, rbsp, rbsp_size);
	(void)h264x_rbr_ue(&br);
	slice_type_raw = h264x_rbr_ue(&br);
	slice_type = slice_type_raw % 5U;
	sp->slice_type = slice_type;
	if (slice_type_raw > 9U)
		return 0;
	if (h264x_rbr_ue(&br) != probe->pps.pic_parameter_set_id)
		return 0;
	(void)h264x_rbr_read_bits(&br, probe->sps.log2_max_frame_num_minus4 + 4U);
	if (probe->sps.pic_order_cnt_type == 0U) {
		(void)h264x_rbr_read_bits(&br, probe->sps.log2_max_pic_order_cnt_lsb_minus4 + 4U);
		if (probe->pps.bottom_field_pic_order_in_frame_present_flag)
			(void)h264x_rbr_se(&br);
	} else if (probe->sps.pic_order_cnt_type == 1U &&
	           !probe->sps.delta_pic_order_always_zero_flag) {
		(void)h264x_rbr_se(&br);
		if (probe->pps.bottom_field_pic_order_in_frame_present_flag)
			(void)h264x_rbr_se(&br);
	}
	if (probe->pps.redundant_pic_cnt_present_flag)
		(void)h264x_rbr_ue(&br);
	if (slice_type == 1U)
		(void)h264x_rbr_read_bit(&br);

	l0_count = probe->pps.num_ref_idx_l0_default_active_minus1;
	l1_count = probe->pps.num_ref_idx_l1_default_active_minus1;
	if (slice_type == 0U || slice_type == 1U || slice_type == 3U) {
		unsigned int override_flag = h264x_rbr_read_bit(&br);
		if (override_flag) {
			l0_count = h264x_rbr_ue(&br);
			if (slice_type == 1U)
				l1_count = h264x_rbr_ue(&br);
		}
	}
	if (l0_count >= 32U || (slice_type == 1U && l1_count >= 32U))
		return 0;

	sp->rplm_start_bit = br.bitpos;
	if (!(slice_type == 2U || slice_type == 4U)) {
		h264x_skip_ref_pic_list_modification(&br, slice_type);
		sp->rplm_end_bit = br.bitpos;
		sp->rplm_replacement_bits = slice_type == 1U ? 2U : 1U;
		sp->changed_rplm =
			(sp->rplm_end_bit > sp->rplm_start_bit + sp->rplm_replacement_bits) ? 1U : 0U;
	} else {
		sp->rplm_end_bit = br.bitpos;
	}

	chroma_array_type = probe->sps.separate_colour_plane_flag ?
		0U : probe->sps.chroma_format_idc;
	has_pred_weight = 0;
	if (probe->pps.weighted_pred_flag &&
	    (slice_type == 0U || slice_type == 3U))
		has_pred_weight = 1;
	if (probe->pps.weighted_bipred_idc == 1U && slice_type == 1U)
		has_pred_weight = 1;
	sp->weight_start_bit = br.bitpos;
	if (has_pred_weight) {
		h264x_skip_pred_weight_table(&br, &probe->sps,
		                            chroma_array_type, l0_count, l1_count,
		                            slice_type == 1U);
		sp->changed_weight = br.error ? 0U : 1U;
	}
	sp->weight_end_bit = br.bitpos;

	sp->marking_start_bit = br.bitpos;
	if (sp->nal_ref_idc != 0U) {
		h264x_skip_dec_ref_pic_marking(&br, sp->nal_unit_type);
		sp->marking_end_bit = br.bitpos;
		sp->marking_replacement_bits = 1U;
		sp->changed_marking =
			(sp->marking_end_bit > sp->marking_start_bit + 1U) ? 1U : 0U;
	} else {
		sp->marking_end_bit = br.bitpos;
	}
	/* Parse the remainder of slice_header so CABAC payload alignment can be
	 * rebuilt after removing or shortening earlier header syntax.  Copying the
	 * old alignment bits after a splice shifts the arithmetic-coded payload and
	 * causes immediate decode failure.  FMO is intentionally rejected here; the
	 * PSP compatibility profiles use one slice group. */
	sp->is_cabac = probe->pps.entropy_coding_mode_flag ? 1U : 0U;
	if (sp->is_cabac && slice_type != 2U && slice_type != 4U)
		(void)h264x_rbr_ue(&br); /* cabac_init_idc */
	(void)h264x_rbr_se(&br); /* slice_qp_delta */
	if (slice_type == 3U || slice_type == 4U) {
		if (slice_type == 3U)
			(void)h264x_rbr_read_bit(&br); /* sp_for_switch_flag */
		(void)h264x_rbr_se(&br); /* slice_qs_delta */
	}
	if (probe->pps.deblocking_filter_control_present_flag) {
		unsigned int disable_deblocking_filter_idc;
		disable_deblocking_filter_idc = h264x_rbr_ue(&br);
		if (disable_deblocking_filter_idc != 1U) {
			(void)h264x_rbr_se(&br);
			(void)h264x_rbr_se(&br);
		}
	}
	if (probe->pps.num_slice_groups_minus1 != 0U)
		return 0;
	if (br.error)
		return 0;
	sp->header_end_bit = br.bitpos;
	sp->cabac_payload_start_bit = br.bitpos;
	if (sp->is_cabac) {
		while ((br.bitpos & 7U) != 0U) {
			/* H.264 cabac_alignment_one_bit shall be equal to 1. */
			if (h264x_rbr_read_bit(&br) != 1U)
				return 0;
		}
		if (br.error)
			return 0;
		sp->cabac_payload_start_bit = br.bitpos;
	}
	return 1;
}

static void h264x_bw_put_rplm_list(struct h264x_bw *bw,
                                    const struct h264x_rplm_op *ops,
                                    unsigned int count)
{
	unsigned int i;

	if (bw == 0)
		return;
	if (count == 0U || ops == 0) {
		h264x_bw_put_bit(bw, 0U);
		return;
	}
	h264x_bw_put_bit(bw, 1U);
	for (i = 0U; i < count; i++) {
		unsigned int idc;
		idc = ops[i].modification_of_pic_nums_idc;
		if (idc > 2U) {
			bw->error = 1;
			return;
		}
		h264x_bw_put_ue(bw, idc);
		if (idc == 0U || idc == 1U)
			h264x_bw_put_ue(bw, ops[i].abs_diff_pic_num_minus1);
		else
			h264x_bw_put_ue(bw, ops[i].long_term_pic_num);
	}
	h264x_bw_put_ue(bw, 3U);
}

static unsigned int h264x_rewrite_nal_ref_select(const struct h264x_probe *probe,
                                                   const uint8_t *nal,
                                                   unsigned int nal_size,
                                                   uint8_t *out,
                                                   unsigned int out_capacity,
                                                   unsigned int *changed_rplm,
                                                   unsigned int *changed_marking,
                                                   unsigned int rewrite_flags,
                                                   const struct h264x_rplm_override *override_plan,
                                                   int *parsed)
{
	uint8_t *rbsp;
	uint8_t *new_rbsp;
	struct h264x_rewrite_splice sp;
	struct h264x_bw bw;
	unsigned int rbsp_size;
	unsigned int new_rbsp_size;
	unsigned int payload_size;
	unsigned int selected_rplm;
	unsigned int selected_weight;
	unsigned int selected_marking;
	unsigned int selected_drop;

	*parsed = 0;
	if (nal == 0 || out == 0 || nal_size < 2U || out_capacity < 2U)
		return 0;
	rbsp = g_h264x_rewrite_rbsp;
	new_rbsp = g_h264x_rewrite_new_rbsp;
	rbsp_size = h264x_payload_to_rbsp(nal + 1U, nal_size - 1U,
	                                 rbsp, PPA_H264X_REWRITE_MAX_RBSP);
	if (rbsp_size == 0U)
		return 0;
	if (!h264x_find_splice_points(probe, rbsp, rbsp_size, nal[0], &sp))
		return 0;

	selected_rplm = ((override_plan != 0 && override_plan->enabled &&
	                 !(sp.slice_type == 2U || sp.slice_type == 4U)) ||
	                 (((rewrite_flags & H264X_REWRITE_RPLM) != 0U) &&
	                  sp.changed_rplm)) ? 1U : 0U;
	selected_weight = ((rewrite_flags & H264X_REWRITE_WEIGHT_TABLE) != 0U &&
	                   sp.changed_weight) ? 1U : 0U;
	selected_drop = ((rewrite_flags & H264X_REWRITE_DROP_REF) != 0U &&
	                 sp.nal_ref_idc != 0U &&
	                 sp.slice_type == 1U) ? 1U : 0U;
	selected_marking = selected_drop ? 1U :
		(((rewrite_flags & H264X_REWRITE_MMCO) != 0U &&
		  sp.changed_marking) ? 1U : 0U);
	if (!selected_rplm && !selected_weight && !selected_marking && !selected_drop) {
		*parsed = 1;
		return 0;
	}

	h264x_bw_init(&bw, new_rbsp, PPA_H264X_REWRITE_MAX_RBSP);
	h264x_bw_copy_bits(&bw, rbsp, 0U, sp.rplm_start_bit);
	if (selected_rplm) {
		if (override_plan != 0 && override_plan->enabled) {
			h264x_bw_put_rplm_list(&bw, override_plan->l0,
			                       override_plan->l0_count);
			if (sp.slice_type == 1U)
				h264x_bw_put_rplm_list(&bw, override_plan->l1,
				                       override_plan->l1_count);
		}
		else if (sp.rplm_replacement_bits == 2U) {
			h264x_bw_put_bit(&bw, 0U);
			h264x_bw_put_bit(&bw, 0U);
		} else if (sp.rplm_replacement_bits == 1U) {
			h264x_bw_put_bit(&bw, 0U);
		}
	} else {
		h264x_bw_copy_bits(&bw, rbsp, sp.rplm_start_bit, sp.rplm_end_bit);
	}

	h264x_bw_copy_bits(&bw, rbsp, sp.rplm_end_bit, sp.weight_start_bit);
	if (!selected_weight)
		h264x_bw_copy_bits(&bw, rbsp, sp.weight_start_bit, sp.weight_end_bit);
	h264x_bw_copy_bits(&bw, rbsp, sp.weight_end_bit, sp.marking_start_bit);

	if (selected_marking) {
		/* A demoted NAL has nal_ref_idc==0, so dec_ref_pic_marking is
		 * absent rather than replaced by adaptive_ref_pic_marking_mode_flag=0. */
		if (!selected_drop && sp.marking_replacement_bits == 1U)
			h264x_bw_put_bit(&bw, 0U);
	} else {
		h264x_bw_copy_bits(&bw, rbsp, sp.marking_start_bit, sp.marking_end_bit);
	}
	if (sp.is_cabac) {
		h264x_bw_copy_bits(&bw, rbsp, sp.marking_end_bit, sp.header_end_bit);
		while ((bw.bitpos & 7U) != 0U)
			h264x_bw_put_bit(&bw, 1U);
		h264x_bw_copy_bits(&bw, rbsp, sp.cabac_payload_start_bit,
		                   rbsp_size * 8U);
	} else {
		h264x_bw_copy_bits(&bw, rbsp, sp.marking_end_bit,
		                   h264x_rbsp_end_bit(rbsp, rbsp_size));
	}
	if (bw.error)
		return 0;

	new_rbsp_size = (bw.bitpos + 7U) >> 3;
	if (new_rbsp_size == 0U)
		return 0;
	out[0] = selected_drop ? (uint8_t)(nal[0] & 0x1FU) : nal[0];
	payload_size = h264x_rbsp_to_payload(new_rbsp, new_rbsp_size,
	                                    out + 1U,
	                                    out_capacity > 0U ? out_capacity - 1U : 0U);
	if (payload_size == 0U)
		return 0;
	if (changed_rplm) *changed_rplm = selected_rplm;
	if (changed_marking) *changed_marking = selected_marking;
	*parsed = 1;

	return payload_size + 1U;
}

static int h264x_rewrite_ref_mgmt_common(const struct h264x_probe *probe,
                                           const void *au_data,
                                           unsigned int au_size,
                                           void *out_data,
                                           unsigned int out_capacity,
                                           unsigned int *out_size,
                                           unsigned int rewrite_flags,
                                           const struct h264x_rplm_override *override_plan)
{
	const uint8_t *src;
	uint8_t *dst;
	unsigned int nal_length_size;
	unsigned int pos;
	unsigned int out_pos;
	unsigned int rewritten;
	unsigned int changed_rplm;
	unsigned int changed_marking;
	const char *fail_reason;

	fail_reason = 0;
	(void)fail_reason;

	if (out_size)
		*out_size = 0;

	if (probe == 0 || au_data == 0 || out_data == 0) {
		fail_reason = "bad_args";
		goto fail;
	}

	if ((rewrite_flags & H264X_REWRITE_ANY) == 0) {
		fail_reason = "no_rewrite_flags";
		goto fail;
	}

	if (!probe->avcc_valid || !probe->sps.valid || !probe->pps.valid ||
	    !probe->current_au.valid || probe->current_au.parse_error) {
		fail_reason = "bad_parameter_sets";
		goto fail;
	}

	if (!probe->current_au.is_p &&
	    !probe->current_au.is_b &&
	    !probe->current_au.is_i) {
		fail_reason = "not_p_b_or_i";
		goto fail;
	}

	/* The dual-DPB plan describes the first slice only. Generic MMCO/weight
	 * removal is parsed independently for every slice; never broadcast a
	 * first-slice reference-list override to differently coded later slices. */
	if (override_plan != 0 && override_plan->enabled &&
	    (probe->current_au.vcl_count != 1U ||
	     override_plan->l0_count > H264X_RPLM_OVERRIDE_MAX_OPS ||
	     override_plan->l1_count > H264X_RPLM_OVERRIDE_MAX_OPS))
		goto fail;

	nal_length_size = probe->nal_length_size;
	if (nal_length_size != 1U && nal_length_size != 2U && nal_length_size != 4U) {
		fail_reason = "bad_nal_length_size";
		goto fail;
	}

	src = (const uint8_t *)au_data;
	dst = (uint8_t *)out_data;
	pos = 0;
	out_pos = 0;
	rewritten = 0;
	changed_rplm = 0;
	changed_marking = 0;

	while (pos <= au_size && nal_length_size <= au_size - pos) {
		unsigned int nal_size;
		unsigned int nal_type;
		unsigned int new_nal_size;
		uint8_t *length_ptr;

		nal_size = h264x_read_nal_length(src + pos, nal_length_size);
		pos += nal_length_size;
		if (nal_size == 0) {
			fail_reason = "zero_nal";
			goto fail;
		}
		if (nal_size > au_size - pos) {
			fail_reason = "nal_overrun";
			goto fail;
		}

		if (out_pos > out_capacity || nal_length_size >= out_capacity - out_pos) {
			fail_reason = "out_capacity_length";
			goto fail;
		}

		length_ptr = dst + out_pos;
		out_pos += nal_length_size;

		nal_type = src[pos] & 0x1FU;
		new_nal_size = 0;
		if (nal_type == H264X_NAL_SLICE_NON_IDR) {
			int parsed;
			if (rewritten != 0U && override_plan != 0 && override_plan->enabled)
				goto fail;
			new_nal_size = h264x_rewrite_nal_ref_select(probe,
			                                            src + pos,
			                                            nal_size,
			                                            dst + out_pos,
			                                            out_capacity - out_pos,
			                                            &changed_rplm,
			                                            &changed_marking,
			                                            rewrite_flags,
			                                            override_plan, &parsed);
			if (!parsed)
				goto fail;
			++rewritten;
		}
		else if (nal_type == H264X_NAL_SLICE_IDR || nal_type == 2U ||
		         nal_type == 3U || nal_type == 4U || nal_type == 7U || nal_type == 8U)
			goto fail;

		if (new_nal_size != 0) {
			if (nal_length_size < 4U && new_nal_size >= (1U << (nal_length_size * 8U)))
				goto fail;
			h264x_write_nal_length(length_ptr, nal_length_size, new_nal_size);
			out_pos += new_nal_size;
		}
		else {
			if (nal_size > out_capacity - out_pos) {
				fail_reason = "out_capacity_payload";
				goto fail;
			}
			h264x_write_nal_length(length_ptr, nal_length_size, nal_size);
			memcpy(dst + out_pos, src + pos, nal_size);
			out_pos += nal_size;
		}

		pos += nal_size;
	}

	if (pos != au_size) {
		fail_reason = "trailing_or_short_au";
		goto fail;
	}

	if (!rewritten) {
		fail_reason = "slice_rewrite_failed";
		goto fail;
	}

	if (out_size)
		*out_size = out_pos;

	return 1;

fail:
	;
	return 0;
}

int h264x_rewrite_ref_mgmt_select(const struct h264x_probe *probe,
                                    const void *au_data,
                                    unsigned int au_size,
                                    void *out_data,
                                    unsigned int out_capacity,
                                    unsigned int *out_size,
                                    unsigned int rewrite_flags)
{
	return h264x_rewrite_ref_mgmt_common(probe, au_data, au_size, out_data,
	                                      out_capacity, out_size, rewrite_flags, 0);
}

int h264x_rewrite_ref_mgmt_override(const struct h264x_probe *probe,
                                      const void *au_data,
                                      unsigned int au_size,
                                      void *out_data,
                                      unsigned int out_capacity,
                                      unsigned int *out_size,
                                      unsigned int rewrite_flags,
                                      const struct h264x_rplm_override *override_plan)
{
	if (override_plan == 0 || !override_plan->enabled)
		return 0;
	return h264x_rewrite_ref_mgmt_common(probe, au_data, au_size, out_data,
	                                      out_capacity, out_size, rewrite_flags,
	                                      override_plan);
}

int h264x_rewrite_ref_mgmt_to_default(const struct h264x_probe *probe,
                                      const void *au_data,
                                      unsigned int au_size,
                                      void *out_data,
                                      unsigned int out_capacity,
                                      unsigned int *out_size)
{
	return h264x_rewrite_ref_mgmt_select(probe,
	                                    au_data,
	                                    au_size,
	                                    out_data,
	                                    out_capacity,
	                                    out_size,
	                                    H264X_REWRITE_ALL);
}

#else
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
                                        unsigned int *out_pps_size)
{
	(void)sps_data; (void)sps_size; (void)pps_data; (void)pps_size;
	(void)target_num_ref_frames; (void)force_level_idc;
	(void)disable_weighted_prediction; (void)out_sps;
	(void)out_sps_capacity; (void)out_pps; (void)out_pps_capacity;
	if (out_sps_size) *out_sps_size = 0U;
	if (out_pps_size) *out_pps_size = 0U;
	return 0;
}

int h264x_rewrite_ref_mgmt_select(const struct h264x_probe *probe,
                                    const void *au_data,
                                    unsigned int au_size,
                                    void *out_data,
                                    unsigned int out_capacity,
                                    unsigned int *out_size,
                                    unsigned int rewrite_flags)
{
	(void)probe;
	(void)au_data;
	(void)au_size;
	(void)out_data;
	(void)out_capacity;
	(void)rewrite_flags;
	if (out_size)
		*out_size = 0;
	return 0;
}

int h264x_rewrite_ref_mgmt_override(const struct h264x_probe *probe,
                                    const void *au_data,
                                    unsigned int au_size,
                                    void *out_data,
                                    unsigned int out_capacity,
                                    unsigned int *out_size,
                                    unsigned int rewrite_flags,
                                    const struct h264x_rplm_override *override_plan)
{
	(void)override_plan;
	return h264x_rewrite_ref_mgmt_select(probe, au_data, au_size, out_data,
	                                     out_capacity, out_size, rewrite_flags);
}

int h264x_rewrite_ref_mgmt_to_default(const struct h264x_probe *probe,
                                      const void *au_data,
                                      unsigned int au_size,
                                      void *out_data,
                                      unsigned int out_capacity,
                                      unsigned int *out_size)
{
	return h264x_rewrite_ref_mgmt_select(probe, au_data, au_size, out_data,
	                                     out_capacity, out_size,
	                                     H264X_REWRITE_ALL);
}

#endif
