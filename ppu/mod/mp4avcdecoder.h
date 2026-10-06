/* 
 *	Copyright (C) 2008 cooleyes
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
 
#ifndef __MP4_AVC_DECODER_H__
#define __MP4_AVC_DECODER_H__

#include <string.h>
#include <pspsdk.h>
#include "pspmpeg.h"
#include "common/mem64.h"

typedef struct {
	ScePVoid sps_buffer;
	SceInt32 sps_size;
	ScePVoid pps_buffer;
	SceInt32 pps_size;
	SceInt32 nal_prefix_size;
	ScePVoid nal_buffer;
	SceInt32 nal_size;
	SceInt32 mode;
} Mp4AvcNalStruct;

typedef struct {
	SceInt32 unknown0;
	SceInt32 unknown1;
	SceInt32 width;
	SceInt32 height;
	SceInt32 unknown4;
	SceInt32 unknown5;
	SceInt32 unknown6;
	SceInt32 unknown7;
	SceInt32 unknown8;
	SceInt32 unknown9;
} Mp4AvcInfoStruct;

typedef struct {
	ScePVoid buffer0;
	ScePVoid buffer1;
	ScePVoid buffer2;
	ScePVoid buffer3;
	ScePVoid buffer4;
	ScePVoid buffer5;
	ScePVoid buffer6;
	ScePVoid buffer7;
	SceInt32 unknown0;
	SceInt32 unknown1;
	SceInt32 unknown2;
} Mp4AvcYuvStruct;

typedef struct {
	SceInt32 unknown0;
	SceInt32 unknown1;
	SceInt32 unknown2;
	SceInt32 unknown3;
	Mp4AvcInfoStruct* info_buffer;
	SceInt32 unknown5;
	SceInt32 unknown6;
	SceInt32 unknown7;
	SceInt32 unknown8;
	SceInt32 unknown9;
	SceInt32 unknown10;
	Mp4AvcYuvStruct* yuv_buffer;
	SceInt32 unknown12;
	SceInt32 unknown13;
	SceInt32 unknown14;
	SceInt32 unknown15;
	SceInt32 unknown16;
	SceInt32 unknown17;
	SceInt32 unknown18;
	SceInt32 unknown19;
	SceInt32 unknown20;
	SceInt32 unknown21;
	SceInt32 unknown22;
	SceInt32 unknown23;
} Mp4AvcDetail2Struct;

typedef struct {
	SceInt32 height;
	SceInt32 width;
	SceInt32 mode0;
	SceInt32 mode1;
	ScePVoid buffer0;
	ScePVoid buffer1;
	ScePVoid buffer2;
	ScePVoid buffer3;
	ScePVoid buffer4;
	ScePVoid buffer5;
	ScePVoid buffer6;
	ScePVoid buffer7;
} Mp4AvcCscStruct;

struct mp4_avc_struct {
	int      mpeg_init;
	int      mpeg_create;
	SceUID   close_thread; /* Reserved before sceMpegInit; dormant until close. */
	ScePVoid mpeg_buffer;
	SceMpeg mpeg;
	SceMpegRingbuffer mpeg_ringbuffer;
	SceMpegAu* mpeg_au;
	SceInt32 mpeg_mode;
	SceInt32 mpeg_buffer_size;
	ScePVoid mpeg_ddrtop;
	ScePVoid mpeg_au_buffer;
	ScePVoid mpeg_sps_pps_buffer;
	SceInt32 mpeg_sps_size;
	SceInt32 mpeg_pps_size;
	SceInt32 mpeg_nal_prefix_size;
	Mp4AvcDetail2Struct* mpeg_detail2;
	SceInt32 mpeg_pic_num;
} ;

#ifdef __cplusplus
extern "C" {
#endif

int mp4_avc_init_ddrtop();
/* Only after playback workers have joined and all AVC contexts are closed. */
void mp4_avc_shutdown(void);
int mp4_avc_is_active(void);
void mp4_avc_safe_constructor(struct mp4_avc_struct *p);
void mp4_avc_close(struct mp4_avc_struct *p);
char *mp4_avc_open(struct mp4_avc_struct *p, int avc_profile, int mpeg_mode, void* sps_buffer, int sps_size, void* pps_buffer, int pps_size, int nal_prefix_size);
char *mp4_avc_get(struct mp4_avc_struct *p, int mode, void *source_buffer, int size, void *destination_buffer, int* pic_num);
char *mp4_avc_get_cache(struct mp4_avc_struct *p, void *destination_buffer, int pic_num);

#ifdef __cplusplus
}
#endif

#endif
