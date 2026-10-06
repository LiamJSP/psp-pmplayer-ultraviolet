/* h264x_compat.c — derived from mkv_decode.c
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
/* Shared policies extracted from the MKV B3 compatibility path. Native AVC
 * submission, source packet order and container timestamp derivation stay with
 * their existing owners. This is not a software decoder or native DPB readback. */
#include "h264x_compat.h"
#include "gu_draw.h"
#include "common/mem64.h"
#include <string.h>

int h264x_two_picture_shape(const struct h264x_picture_identity *a,
    const struct h264x_picture_identity *b, int frame_duration)
{
    int64_t delta;
    int poc_delta;
    unsigned int modulus;
    if (!a || !b || !a->valid || !b->valid ||
        !a->is_b || !a->is_reference_b || !b->is_b || b->is_reference_b ||
        a->poc_lsb > 65535U || b->poc_lsb > 65535U)
        return 0;
    if (frame_duration <= 0) frame_duration = 33;
    delta = (int64_t)b->timestamp - a->timestamp;
    if (delta < frame_duration - 2 || delta > frame_duration + 2) return 0;
    poc_delta = (int)b->poc_lsb - (int)a->poc_lsb;
    if (poc_delta == 2) return 1;
    for (modulus = 16U; modulus <= 65536U; modulus <<= 1U)
        if (poc_delta == 2 - (int)modulus) return 1;
    return 0;
}

int h264x_two_picture_prepare(struct h264x_two_picture_remap *s,
    struct mp4_avc_struct *avc, void *rgb, unsigned int pitch, unsigned int height)
{
    unsigned int bytes;
    if (!s || !avc || !rgb || (pitch != 512U && pitch != 768U) ||
        !height || height > 480U || avc->mpeg_pic_num != 2) return 0;
    if (s->pending_slot0) { s->pending_slot0 = 0U; return 0; }
    bytes = pitch * height * 4U;
    if (s->saved_bytes < bytes || !s->saved_slot0) {
        void *replacement = malloc_64(bytes);
        if (!replacement) return 0;
        if (s->saved_slot0) free_64(s->saved_slot0);
        s->saved_slot0 = replacement;
        s->saved_bytes = bytes;
    }
    /* GE -> GE copies retain the established device ownership path. Each
     * copy is joined before CSC/slot reuse; no CPU reads stale RGB pixels. */
    if (ppa_gu_copy_frame_blocking(s->saved_slot0, pitch, rgb, pitch, pitch, height) < 0)
        return 0;
    if (mp4_avc_get_cache(avc, rgb, 1) != 0) {
        return ppa_gu_copy_frame_blocking(rgb, pitch, s->saved_slot0,
            pitch, pitch, height) < 0 ? -1 : 0;
    }
    s->pending_slot0 = 1U;
    return 1;
}

int h264x_two_picture_restore(struct h264x_two_picture_remap *s,
    void *rgb, unsigned int pitch, unsigned int height, unsigned int pic_num)
{
    if (!s || !s->pending_slot0) return 0;
    s->pending_slot0 = 0U;
    if (pic_num != 1U || !s->saved_slot0 || !rgb ||
        (pitch != 512U && pitch != 768U) || !height || height > 480U ||
        pitch * height * 4U > s->saved_bytes) return -1;
    return ppa_gu_copy_frame_blocking(rgb, pitch, s->saved_slot0,
        pitch, pitch, height) < 0 ? -1 : 1;
}

void h264x_two_picture_release(struct h264x_two_picture_remap *s)
{
    if (!s) return;
    if (s->saved_slot0) free_64(s->saved_slot0);
    memset(s, 0, sizeof(*s));
}
