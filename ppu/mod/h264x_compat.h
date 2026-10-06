/* h264x_compat.h — derived from mkv_decode.c
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
#ifndef PPA_H264X_COMPAT_H
#define PPA_H264X_COMPAT_H

#include "h264x_rewrite.h"
#include "mp4avcdecoder.h"

#ifndef PPA_H264X_DEEP_PRESERVE_RPLM
#define PPA_H264X_DEEP_PRESERVE_RPLM 1
#endif
#ifndef PPA_H264X_DEEP_B_POC_DELTA_THRESHOLD
#define PPA_H264X_DEEP_B_POC_DELTA_THRESHOLD 8U
#endif

#define PPA_H264X_COMPAT_CABAC_DPB     0x01U
#define PPA_H264X_COMPAT_DEEP_DPB      0x02U
#define PPA_H264X_COMPAT_WEIGHTED_PRED 0x04U
#define PPA_H264X_COMPAT_LEVEL_RETRY   0x08U

struct h264x_compat_selection {
    unsigned int flags, original_refs, effective_refs, disable_weights;
};

/* Container-independent initial policy. Geometry still selects the native
 * context; either adapter may use mode 5 when this policy needs normalization. */
static inline void h264x_compat_select(const struct h264x_probe *p,
                                      struct h264x_compat_selection *s)
{
    s->flags = s->original_refs = s->effective_refs = s->disable_weights = 0U;
    if (!p || !p->sps.valid || !p->pps.valid) return;
    s->original_refs = p->sps.num_ref_frames;
    s->effective_refs = s->original_refs;
    if (s->original_refs > 3U) {
        s->flags |= PPA_H264X_COMPAT_DEEP_DPB;
        if (p->pps.entropy_coding_mode_flag) s->flags |= PPA_H264X_COMPAT_CABAC_DPB;
        s->effective_refs = 3U;
    }
    if (p->pps.weighted_pred_flag || p->pps.weighted_bipred_idc)
        s->flags |= PPA_H264X_COMPAT_WEIGHTED_PRED;
    if (s->flags && !s->effective_refs) s->effective_refs = 1U;
    s->disable_weights = h264x_disable_weights_on_open(p);
}

/* MKV's shallow reference-management bridge also applies to unweighted B3
 * streams that need no SPS/PPS normalization. Deep-list/dual-DPB decisions
 * remain adapter inputs. The packet is always immutable; flags address only
 * a complete decoder-facing copy. Zero means no pre-rewrite is needed. */
static inline unsigned int h264x_ref_bridge_flags(const struct h264x_probe *p,
    unsigned int effective_refs, unsigned int flatten_brefs,
    int preserve_deep_lists, int weights_required)
{
    unsigned int flags = H264X_REWRITE_MMCO;
    int needed = weights_required;
    int preserve = preserve_deep_lists ||
        h264x_preserve_weighted_p_lists(p, effective_refs, flatten_brefs);
    const struct h264x_au_info *a = &p->current_au;
    if (!preserve && !a->mmco_nonzero) flags |= H264X_REWRITE_RPLM;
    if (p->reference_b_count &&
        (a->is_p || a->is_b || (PPA_H264X_REWRITE_I_MMCO && a->is_i && a->mmco_nonzero)) &&
        (a->mmco_nonzero || (!preserve &&
            (a->ref_pic_list_modification_flag_l0 || a->ref_pic_list_modification_flag_l1))))
        needed = 1;
    if (weights_required) flags |= H264X_REWRITE_WEIGHT_TABLE;
    if (flatten_brefs && a->is_reference_b) {
        flags |= H264X_REWRITE_DROP_REF | H264X_REWRITE_RPLM;
        needed = 1;
    }
    return needed ? flags : 0U;
}

/* Identity of a queued compressed picture, in presentation timestamp order.
 * Deliberately independent of container DTS, sample tables and block layout. */
struct h264x_picture_identity {
    int valid, timestamp;
    unsigned int poc_lsb, is_b, is_reference_b;
};

struct h264x_two_picture_remap {
    void *saved_slot0;
    unsigned int saved_bytes, pending_slot0;
};

#ifdef __cplusplus
extern "C" {
#endif
int h264x_two_picture_shape(const struct h264x_picture_identity *a,
    const struct h264x_picture_identity *b, int frame_duration);
/* Producer-owned; no display/consumer thread may access the saved RGB slot.
 * Return 1 for remap, 0 for native fallback, -1 for unrecoverable copy failure. */
int h264x_two_picture_prepare(struct h264x_two_picture_remap *state,
    struct mp4_avc_struct *avc, void *rgb, unsigned int pitch, unsigned int height);
int h264x_two_picture_restore(struct h264x_two_picture_remap *state,
    void *rgb, unsigned int pitch, unsigned int height, unsigned int pic_num);
void h264x_two_picture_release(struct h264x_two_picture_remap *state);
#ifdef __cplusplus
}
#endif
#endif
