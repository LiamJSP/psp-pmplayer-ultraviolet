/* SPDX-License-Identifier: GPL-2.0-or-later
 * Packed PCM conversion on the existing decode producer. No worker, scratchpad,
 * temporary sample array, allocator, or independent resampling phase.
 */
#include "audio_pcm_vfpu.h"
#include <samplerate.h>
#include <stdint.h>

#ifndef PPA_ENABLE_VFPU_ACCEL
#define PPA_ENABLE_VFPU_ACCEL 1
#endif
#ifndef PPA_ENABLE_VFPU_AUDIO
#define PPA_ENABLE_VFPU_AUDIO 1
#endif
#ifndef PPU_AUDIO_VFPU_PCM
#define PPU_AUDIO_VFPU_PCM 1
#endif
#define PPU_PCM_VFPU_ENABLED \
    (PPA_ENABLE_VFPU_ACCEL && PPA_ENABLE_VFPU_AUDIO && PPU_AUDIO_VFPU_PCM)

int ppu_audio_pcm_vfpu_enabled(void)
{
    return PPU_PCM_VFPU_ENABLED != 0;
}

#if PPU_PCM_VFPU_ENABLED
/* Source/target identity: x | (y << 2) | (z << 4) | (w << 6), with
 * abs/constant/negate bits clear. Write the control registers directly:
 * some PSP assemblers reject the bracketed vpfxs/vpfxt operand spelling.
 * Zero is NOT a source identity (it would broadcast the x lane).
 */
#define PPU_VFPU_PREFIX_IDENTITY 0xE4U

/* Normalized signed-16 bounds, both exactly representable in binary32.
 * Clamp BEFORE rounding and shifting into vi2s's high-halfword format: +1
 * must become +32767, never a wrapped -32768 or an overflowing int32.
 */
static const float pcm_bounds[8] __attribute__((aligned(16))) = {
    -1.0f, -1.0f, -1.0f, -1.0f,
    32767.0f / 32768.0f, 32767.0f / 32768.0f,
    32767.0f / 32768.0f, 32767.0f / 32768.0f
};

/* Full batches stay inside one asm statement, including loop control. GCC
 * does not allocate VFPU registers; the public call boundary above declares
 * their ownership. No C calls or scalar FPU work occur inside these kernels.
 * Early-clobber pointer/count operands cannot alias still-live input operands.
 * Reset all prefixes on entry and leave them at identity. The memory clobber
 * plus vsync/vflush orders VFPU stores before the caller's CPU reads/memmove.
 * lv.s/sv.s handle word-aligned PCM; lv.q/sv.q only see aligned floats.
 * Never use ulv.q/lvl.q/lvr.q: PSP-1000 has a scalar-FPU corruption erratum.
 */
static void pcm_to_float_batches(const short *in, float *out, int batches)
{
    __asm__ volatile(
        ".set push\n"
        ".set noreorder\n"
        "mtvc %[identity], $128\n"
        "mtvc %[identity], $129\n"
        "mtvc $0, $130\n"
        "1:\n"
        "lv.s S000, 0(%0)\n"
        "lv.s S001, 4(%0)\n"
        "lv.s S002, 8(%0)\n"
        "lv.s S003, 12(%0)\n"
        /* Two packed words -> four signed integers, each shifted left 16.
         * Division by 2^31 is exactly sample / 32768 for every s16 value. */
        "vs2i.p C100, C000\n"
        "vs2i.p C110, C002\n"
        "vi2f.q C100, C100, 31\n"
        "vi2f.q C110, C110, 31\n"
        "sv.q C100, 0(%1)\n"
        "sv.q C110, 16(%1)\n"
        "addiu %0, %0, 16\n"
        "addiu %1, %1, 32\n"
        "addiu %2, %2, -1\n"
        "bnez %2, 1b\n"
        "nop\n"
        "vsync\n"
        "vflush\n"
        ".set pop\n"
        : "+&r"(in), "+&r"(out), "+&r"(batches)
        : [identity] "r"(PPU_VFPU_PREFIX_IDENTITY)
        : "memory");
}

static void float_to_pcm_batches(const float *in, short *out, int batches)
{
    __asm__ volatile(
        ".set push\n"
        ".set noreorder\n"
        "mtvc %[identity], $128\n"
        "mtvc %[identity], $129\n"
        "mtvc $0, $130\n"
        "lv.q C200, 0(%3)\n"
        "lv.q C210, 16(%3)\n"
        "1:\n"
        "lv.q C000, 0(%0)\n"
        "lv.q C010, 16(%0)\n"
        "vmax.q C000, C000, C200\n"
        "vmax.q C010, C010, C200\n"
        "vmin.q C000, C000, C210\n"
        "vmin.q C010, C010, C210\n"
        /* Round to the s16 lattice first (nearest, ties to even). vi2s only
         * extracts high halfwords; vf2in(...,31) followed directly by vi2s
         * would discard those fractional bits and incorrectly floor PCM. */
        "vf2in.q C000, C000, 15\n"
        "vf2in.q C010, C010, 15\n"
        "vi2f.q C000, C000, 0\n"
        "vi2f.q C010, C010, 0\n"
        /* Exact integers in [-32768,32767]; scaling by 2^16 cannot overflow. */
        "vf2iz.q C000, C000, 16\n"
        "vf2iz.q C010, C010, 16\n"
        "vi2s.q C100, C000\n"
        "vi2s.q C110, C010\n"
        "sv.s S100, 0(%1)\n"
        "sv.s S101, 4(%1)\n"
        "sv.s S110, 8(%1)\n"
        "sv.s S111, 12(%1)\n"
        "addiu %0, %0, 32\n"
        "addiu %1, %1, 16\n"
        "addiu %2, %2, -1\n"
        "bnez %2, 1b\n"
        "nop\n"
        "vsync\n"
        "vflush\n"
        ".set pop\n"
        : "+&r"(in), "+&r"(out), "+&r"(batches)
        : "r"(pcm_bounds), [identity] "r"(PPU_VFPU_PREFIX_IDENTITY)
        : "memory");
}

static int scalar_rounds_to_nearest(void)
{
    unsigned int fcsr;
    __asm__ volatile("cfc1 %0, $31\n" : "=r"(fcsr));
    /* libsamplerate's lrintf follows CP1 rounding. Only substitute the fixed
     * VFPU nearest mode when it agrees; never modify the caller's FCSR. */
    return (fcsr & 3U) == 0U;
}
#endif

void ppu_audio_pcm_to_float(const short *in, float *out, int samples)
{
    if (samples <= 0) return;
#if PPU_PCM_VFPU_ENABLED
    if (samples >= 8) {
        int head = (int)(((16U - ((uintptr_t)out & 15U)) & 15U) / sizeof(float));
        /* A scalar head aligns floats without assuming the independently
         * trimmed PCM cursor shares their alignment. Odd sample offsets can
         * leave PCM halfword-aligned; those retain the scalar helper. */
        if (samples - head >= 8 && !((uintptr_t)(in + head) & 3U)) {
            int bulk;
            if (head) src_short_to_float_array(in, out, head);
            in += head; out += head; samples -= head;
            bulk = samples & ~7;
            pcm_to_float_batches(in, out, bulk / 8);
            in += bulk; out += bulk; samples -= bulk;
        }
    }
#endif
    if (samples) src_short_to_float_array(in, out, samples);
}

void ppu_audio_float_to_pcm(const float *in, short *out, int samples)
{
    if (samples <= 0) return;
#if PPU_PCM_VFPU_ENABLED
    if (samples >= 8 && scalar_rounds_to_nearest()) {
        int head = (int)(((16U - ((uintptr_t)in & 15U)) & 15U) / sizeof(float));
        if (samples - head >= 8 && !((uintptr_t)(out + head) & 3U)) {
            int bulk;
            if (head) src_float_to_short_array(in, out, head);
            in += head; out += head; samples -= head;
            bulk = samples & ~7;
            float_to_pcm_batches(in, out, bulk / 8);
            in += bulk; out += bulk; samples -= bulk;
        }
    }
#endif
    if (samples) src_float_to_short_array(in, out, samples);
}
