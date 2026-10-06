/* 
 *	Copyright (C) 2006 cooleyes
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
 
#ifndef __CODEC_PRX__
#define __CODEC_PRX__

#include <pspsdk.h>

#ifdef __cplusplus
extern "C" {
#endif

char *load_codec_prx(const char* ppa_path, int devkitVersion);
/* Close firmware codecs before potentially blocking reader/stat-file teardown.
 * Call only from the decoder's exclusive owner or after its workers joined.
 * Reader metadata and software H264X state remain available to the caller. */
struct mp4_avc_struct;
void media_codecs_close(struct mp4_avc_struct *avc, int *audio_state);
/* Application shutdown only; no open codec and no in-flight workers. */
int unload_codec_prx(void);

#ifdef __cplusplus
}
#endif

#endif
