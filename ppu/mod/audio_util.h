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
 
#ifndef __AUDIO_UTIL__
#define __AUDIO_UTIL__

#include <pspsdk.h>

#ifdef __cplusplus
extern "C" {
#endif

void pcm_set_normalize_ratio(unsigned int ratio_type);
void pcm_normalize(short *pcm_buffer, unsigned int number_of_samples);
void pcm_select_channel(short *pcm_buffer, unsigned int number_of_samples, int channel);
void pcm_up_sample(short *dest_pcm_buffer, short *src_pcm_buffer, int up_sample, unsigned int number_of_samples);
void pcm_resample_stereo_linear(short *dest_pcm_buffer,
                                const short *src_pcm_buffer,
                                unsigned int input_frames,
                                unsigned int output_frames);

#ifdef __cplusplus
}
#endif

#endif
