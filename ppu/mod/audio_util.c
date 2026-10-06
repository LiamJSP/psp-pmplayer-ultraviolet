/* 
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
 
#include "audio_util.h"
#include "audio_vfpu.h"
#include "cpu_clock.h"
#include <stdint.h>

static int pcm_normalize_ratio = 0;

void pcm_set_normalize_ratio(unsigned int ratio_type) {
    /* Public input is 0..6. Clamp before calculating a shift count. */
    if (ratio_type > 6U) ratio_type = 6U;
    pcm_normalize_ratio = (int)ratio_type - 3;
}

void pcm_normalize(short *pcm_buffer, unsigned int number_of_samples) {
    /* Snapshot once per block: controller updates cannot change gain halfway
     * through a stereo block. Multiplication is defined for negative samples;
     * left-shifting signed negative PCM was undefined C behavior. */
    int ratio = pcm_normalize_ratio;
    if (ratio > 0) {
        int gain = 1 << ratio; /* bounded 2..8, products fit int32 */
        while (number_of_samples--) {
            int sample = (int)*pcm_buffer * gain;
            if (sample > 32767) sample = 32767;
            else if (sample < -32768) sample = -32768;
            *pcm_buffer++ = (short)sample;
        }
    } else if (ratio < 0) {
        /* Explicit floor division preserves Allegrex arithmetic-right-shift
         * rounding without implementation-defined signed shifts. */
        unsigned int shift = (unsigned int)-ratio;
        int bias = (1 << shift) - 1;
        while (number_of_samples--) {
            int sample = *pcm_buffer;
            sample = sample < 0 ? -(((-sample) + bias) >> shift) :
                                  (sample >> shift);
            *pcm_buffer++ = (short)sample;
        }
    }
}

void pcm_select_channel(short *pcm_buffer, unsigned int number_of_samples, int channel) {
	if (channel != 0) {
		if(channel == -1) {
			while(number_of_samples) {
				*(pcm_buffer+1) = *pcm_buffer;
				pcm_buffer+=2;
				number_of_samples-=2;
			}
		}
		else {
			while(number_of_samples) {
				*pcm_buffer = *(pcm_buffer+1);
				pcm_buffer+=2;
				number_of_samples-=2;
			}
		}
	}
}

void pcm_up_sample(short *dest_pcm_buffer, short *src_pcm_buffer, int up_sample, unsigned int number_of_samples) {
	unsigned int i;
	int j, k, count;
	count = 2*(up_sample+1);
	k = 0;
	for(i = 0; i < number_of_samples; i++) {
		for(j=0; j< count; j++) {
			dest_pcm_buffer[k] = (k%2==0) ? src_pcm_buffer[i<<1] : src_pcm_buffer[(i<<1)+1];
			k++;
		}
	}
}

/*
 * Stereo resampler entry point for the small set of PSP output-rate
 * conversions admitted by the container layer. The generic fallback below has
 * no heap allocation or floating point and keeps the first/last sample exact;
 * Phase 4 may satisfy the call through its session plan/worker first.
 */
void pcm_resample_stereo_linear(short *dest_pcm_buffer,
                                const short *src_pcm_buffer,
                                unsigned int input_frames,
                                unsigned int output_frames) {
	unsigned int out;
	uint64_t started;
	unsigned int elapsed;
	uint64_t step;
	uint64_t phase = 0;

	/* Phase 4 owns only real, session-declared sample-rate conversions. It
	 * starts scalar and may move the work to an isolated VFPU worker only after
	 * measuring the complete scheduling/context cost. */
	if (ppa_audio_accel_resample(dest_pcm_buffer, src_pcm_buffer,
	                             input_frames, output_frames))
		return;

	if (dest_pcm_buffer == 0 || src_pcm_buffer == 0 ||
	    input_frames == 0 || output_frames == 0)
		return;

	/* Allocation/session failures retain the legacy generic scalar path and
	 * still publish its real duty to the CPU governor. */
	started = cpu_clock_auto_now_us();
	if (input_frames == 1 || output_frames == 1) {
		for (out = 0; out < output_frames; ++out) {
			dest_pcm_buffer[out * 2] = src_pcm_buffer[0];
			dest_pcm_buffer[out * 2 + 1] = src_pcm_buffer[1];
		}
	}
	else {
		step = (((uint64_t)(input_frames - 1)) << 32) /
		       (uint64_t)(output_frames - 1);

		for (out = 0; out < output_frames; ++out) {
			unsigned int index;
			if (out + 1U == output_frames) {
				dest_pcm_buffer[out * 2] =
					src_pcm_buffer[(input_frames - 1U) * 2U];
				dest_pcm_buffer[out * 2 + 1U] =
					src_pcm_buffer[(input_frames - 1U) * 2U + 1U];
				break;
			}
			index = (unsigned int)(phase >> 32);
			{
				unsigned int fraction =
					(unsigned int)(phase & 0xffffffffULL);
				unsigned int next = index + 1 < input_frames ?
					index + 1 : index;
				int channel;

				for (channel = 0; channel < 2; ++channel) {
					int32_t a =
						src_pcm_buffer[index * 2 + channel];
					int32_t b =
						src_pcm_buffer[next * 2 + channel];
					int64_t delta =
						(int64_t)(b - a) * (int64_t)fraction;
					dest_pcm_buffer[out * 2 + channel] =
						(short)(a + (int32_t)(delta >> 32));
				}
			}
			phase += step;
		}
	}
	elapsed = (unsigned int)(cpu_clock_auto_now_us() - started);
	cpu_clock_auto_on_audio_resample_us(elapsed);
}

