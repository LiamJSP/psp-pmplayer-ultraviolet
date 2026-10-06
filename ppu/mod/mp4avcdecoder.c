/* mp4avcdecoder.c
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

#include "mp4avcdecoder.h"
#include "../common/ppa_cache.h"
#include "../common/ppa_wait.h"
#include "../common/ppa_video_limits.h"
extern int sceMpegGetAvcNalAu(SceMpeg *mpeg, Mp4AvcNalStruct *nal, SceMpegAu *au);
extern int sceMpegAvcDecodeDetail2(SceMpeg *mpeg, void *detail2);
extern int sceMpegBaseCscAvc(void *destination, int unknown, int width, void *csc);
#include <stdlib.h>
#include <stdio.h>

static ScePVoid DDRTOP = 0;
/* There is one firmware AVC instance. Owner-thread transitions plus the close
 * worker join serialize this pointer; callbacks never inspect or change it. */
static struct mp4_avc_struct *g_avc_owner;

#ifndef PPA_H264X_AVC_CSC_CACHE_HARDEN
#define PPA_H264X_AVC_CSC_CACHE_HARDEN 0
#endif

#if PPA_H264X_AVC_CSC_CACHE_HARDEN
static unsigned int mp4_avc_cache_barrier_events = 0;

static void mp4_avc_cache_barrier(const char *fn,
                                  const char *stage,
                                  struct mp4_avc_struct *p,
                                  void *dst) {
	ppa_cache_wbinv_all();
	mp4_avc_cache_barrier_events++;
}
#else
static void mp4_avc_cache_barrier(const char *fn,
                                  const char *stage,
                                  struct mp4_avc_struct *p,
                                  void *dst) {
	(void)fn; (void)stage; (void)p; (void)dst;
}
#endif

int mp4_avc_init_ddrtop() {
	if ( DDRTOP == 0 ) {
		DDRTOP = memalign(0x400000, 0x200000);
	}
	if ( DDRTOP )
		return 1;
	else
		return 0;
}

void mp4_avc_safe_constructor(struct mp4_avc_struct *p) {
	memset(p, 0, sizeof(*p));
	p->mpeg_init = -1;
	p->mpeg_create = -1;
	p->close_thread = -1;
}

/* Only the exclusive decoder owner, or its joined close worker, calls this.
 * PSPSDK declares Delete/Finish void: returning is the available completion
 * contract, not a hardware-state inspection or an invented status code. */
static void mp4_avc_release_firmware(struct mp4_avc_struct *p)
{
    if (p->mpeg_create == 0) {
        sceMpegDelete(&p->mpeg);
        p->mpeg_create = -1;
    }
    if (p->mpeg_init == 0) {
        sceMpegFinish();
        p->mpeg_init = -1;
    }
}

static int mp4_avc_close_worker(SceSize args, void *argp)
{
    struct mp4_avc_struct *p = *(struct mp4_avc_struct **)argp;
    (void)args;
    mp4_avc_release_firmware(p);
    return 0;
}

int mp4_avc_is_active(void) { return g_avc_owner != 0; }

void mp4_avc_shutdown(void)
{
    if (g_avc_owner != 0)
        ppa_wait_quarantine("AVC still owned at application shutdown", -1);
    if (DDRTOP != 0) {
        free(DDRTOP); /* Allocated by memalign, not malloc_64. */
        DDRTOP = 0;
    }
}

void mp4_avc_close(struct mp4_avc_struct *p) {
    if (p == 0) return;
    if (g_avc_owner == p && p->mpeg_create != 0 && p->mpeg_init != 0)
        ppa_wait_quarantine("AVC ownership flags lost", -1);
    if (p->mpeg_create == 0 || p->mpeg_init == 0) {
        int result;
        if (g_avc_owner != p)
            ppa_wait_quarantine("AVC close owner mismatch", -1);
        result = p->close_thread < 0 ? -1 :
                 sceKernelStartThread(p->close_thread, sizeof(p), &p);
        if (result < 0) {
            if (p->close_thread >= 0) {
                SceKernelThreadInfo info;
                memset(&info, 0, sizeof(info));
                info.size = sizeof(info);
                if (sceKernelReferThreadStatus(p->close_thread, &info) < 0 ||
                    !(info.status & PSP_THREAD_STOPPED) ||
                    (info.status & PSP_THREAD_KILLED))
                    ppa_wait_quarantine("Sony close worker ownership uncertain", result);
            }
            /* No close worker started, and the caller already owns a quiescent
             * decoder. Run its normal teardown here rather than demand a power
             * cycle for a thread-start failure. A stuck firmware call still
             * retains this owner and its buffers; HOME only queues intent. */
            mp4_avc_release_firmware(p);
        } else {
            result = ppa_wait_thread_end_safe(p->close_thread, "AVC", "sony_close",
                                              2000000U, 250000U);
            if (result < 0) ppa_wait_quarantine("Sony close owner failed", result);
        }
        if (p->mpeg_create == 0 || p->mpeg_init == 0)
            ppa_wait_quarantine("Sony close incomplete", -1);
        g_avc_owner = 0;
    }
    if (p->close_thread >= 0) sceKernelDeleteThread(p->close_thread);

	if (p->mpeg_buffer != 0) 
		free_64(p->mpeg_buffer);
	
	if (p->mpeg_au != 0) 
		free_64(p->mpeg_au);
	
	/* The shared DDRTOP arena survives seeks; only application shutdown frees it. */
	
	if (p->mpeg_sps_pps_buffer != 0) 
		free_64(p->mpeg_sps_pps_buffer);

	mp4_avc_safe_constructor(p);
}

char *mp4_avc_open(struct mp4_avc_struct *p, int avc_profile, int mpeg_mode, void* sps_buffer, int sps_size, void* pps_buffer, int pps_size, int nal_prefix_size) {

	(void)avc_profile;
	if (p == 0)
		return("mp4_avc_open: null decoder");
	if (g_avc_owner != 0)
		return("mp4_avc_open: previous decoder still owned");
	mp4_avc_safe_constructor(p);
	if (sps_buffer == 0 || pps_buffer == 0 || sps_size <= 0 || pps_size <= 0 ||
	    sps_size > 0x7fffffff - pps_size || nal_prefix_size <= 0)
		return("mp4_avc_open: invalid AVC parameter set");
	
	p->mpeg_sps_size = sps_size;
	p->mpeg_pps_size = pps_size;
	p->mpeg_nal_prefix_size = nal_prefix_size;
	p->mpeg_sps_pps_buffer = malloc_64(sps_size + pps_size);
	if ( p->mpeg_sps_pps_buffer == 0 ) {
		mp4_avc_close(p);
		return("mp4_avc_open: malloc_64 failed on mpeg_sps_pps_buffer");
	}
	memcpy(p->mpeg_sps_pps_buffer, sps_buffer, sps_size);
	memcpy((unsigned char *)p->mpeg_sps_pps_buffer + sps_size, pps_buffer, pps_size);

	p->mpeg_mode = mpeg_mode;
	/* Reserve teardown resources before acquiring firmware state. Failure now
	 * is an ordinary open error, not an allocation failure after playback. */
	p->close_thread = sceKernelCreateThread("ppa_sony_close", mp4_avc_close_worker,
	                                       0x18, 0x8000, PSP_THREAD_ATTR_USER, 0);
	if (p->close_thread < 0) {
		mp4_avc_close(p);
		return("mp4_avc_open: cannot reserve close worker");
	}
	p->mpeg_init = sceMpegInit();
	if (p->mpeg_init != 0) {
		mp4_avc_close(p);
		return("mp4_avc_open: sceMpegInit failed");
	}
	g_avc_owner = p;
	
	p->mpeg_buffer_size = sceMpegQueryMemSize(p->mpeg_mode);
	if (p->mpeg_buffer_size < 0) {
		mp4_avc_close(p);
		return("mp4_avc_open: sceMpegQueryMemSize failed");
	}
	
	p->mpeg_buffer = malloc_64(p->mpeg_buffer_size);
	if (p->mpeg_buffer == 0) {
		mp4_avc_close(p);
		return("mp4_avc_open: malloc_64 failed on mpeg_buffer");
	}
	
	if ( DDRTOP == 0 ) {
		DDRTOP = memalign(0x400000, 0x200000);
	}
	p->mpeg_ddrtop =  DDRTOP;//memalign(0x400000, 0x200000);
	if (p->mpeg_ddrtop == 0) {
		mp4_avc_close(p);
		return("mp4_avc_open: memalign(0x400000, 0x200000) failed on mpeg_ddrtop");
	}
	p->mpeg_au_buffer = (unsigned char *)p->mpeg_ddrtop + 0x10000;

	p->mpeg_create = sceMpegCreate(&p->mpeg, p->mpeg_buffer, p->mpeg_buffer_size, &p->mpeg_ringbuffer, 512, p->mpeg_mode, (SceInt32)p->mpeg_ddrtop);
	if (p->mpeg_create != 0) {
		mp4_avc_close(p);
		return("mp4_avc_open: sceMpegCreate failed");
	}
	
	p->mpeg_au = (SceMpegAu*)malloc_64(64);
	if (p->mpeg_au == 0) {
		mp4_avc_close(p);
		return("mp4_avc_open: malloc_64 failed on mpeg_au");
	}
	memset(p->mpeg_au, 0xFF, 64);
	
	int au_result;
	au_result = sceMpegInitAu(&p->mpeg, p->mpeg_au_buffer, p->mpeg_au);
	if (au_result != 0) {
		mp4_avc_close(p);
		return("mp4_avc_open: sceMpegInitAu failed");
	}

	return(0);
}

/* Fresh and delayed pictures use the same firmware CSC descriptor. Callers
 * validate geometry and retain their own cache/ownership boundaries. */
static inline void mp4_avc_csc_setup(Mp4AvcCscStruct *csc,
                                    const Mp4AvcInfoStruct *info,
                                    const Mp4AvcYuvStruct *yuv)
{
	csc->height = (info->height + 15) >> 4;
	csc->width = (info->width + 15) >> 4;
	csc->mode0 = 0;
	csc->mode1 = 0;
	csc->buffer0 = yuv->buffer0;
	csc->buffer1 = yuv->buffer1;
	csc->buffer2 = yuv->buffer2;
	csc->buffer3 = yuv->buffer3;
	csc->buffer4 = yuv->buffer4;
	csc->buffer5 = yuv->buffer5;
	csc->buffer6 = yuv->buffer6;
	csc->buffer7 = yuv->buffer7;
}

char *mp4_avc_get(struct mp4_avc_struct *p, int mode, void *source_buffer, int size, void *destination_buffer, int* pic_num) {

	Mp4AvcNalStruct nal;
	int call_result;

	if (p == 0 || source_buffer == 0 || destination_buffer == 0 ||
	    pic_num == 0 || size <= 0 || p->mpeg_create != 0 || p->mpeg_au == 0 ||
	    p->mpeg_sps_pps_buffer == 0)
		return("avc_get: invalid decoder state or arguments");

	nal.sps_buffer = p->mpeg_sps_pps_buffer;
	nal.sps_size = p->mpeg_sps_size;
	nal.pps_buffer = (unsigned char *)p->mpeg_sps_pps_buffer + p->mpeg_sps_size;
	nal.pps_size = p->mpeg_pps_size;
	nal.nal_prefix_size = p->mpeg_nal_prefix_size;
	nal.nal_buffer = source_buffer;
	nal.nal_size = size;
	nal.mode = mode;

	mp4_avc_cache_barrier("get", "before_get_au", p, destination_buffer);
	call_result = sceMpegGetAvcNalAu(&p->mpeg, &nal, p->mpeg_au);
	if (call_result != 0) {
		return("avc_get: sceMpegGetAvcNalAu failed");
	}

	mp4_avc_cache_barrier("get", "before_decode", p, destination_buffer);
	call_result = sceMpegAvcDecode(&p->mpeg, p->mpeg_au, 512, 0,
	                               &p->mpeg_pic_num);
	if (call_result != 0) {
		return("avc_get: sceMpegAvcDecode failed");
	}

	mp4_avc_cache_barrier("get", "after_decode_before_detail2", p, destination_buffer);
	call_result = sceMpegAvcDecodeDetail2(&p->mpeg, &p->mpeg_detail2);
	if (call_result != 0 || (p->mpeg_pic_num > 0 &&
	    (p->mpeg_detail2 == 0 || p->mpeg_detail2->info_buffer == 0 ||
	     p->mpeg_detail2->yuv_buffer == 0))) {
		return call_result != 0 ?
			("avc_get: sceMpegAvcDecodeDetail2 failed") :
			("avc_get: invalid AVC detail metadata");
	}
	mp4_avc_cache_barrier("get", "after_detail2", p, destination_buffer);
	if (p->mpeg_pic_num > 0) {
		Mp4AvcCscStruct csc;
		Mp4AvcInfoStruct *info = p->mpeg_detail2->info_buffer;
		Mp4AvcYuvStruct *yuv = p->mpeg_detail2->yuv_buffer;
		int csc_width;

		if (!PPA_VIDEO_DIMENSIONS_VALID(info->width, info->height)) {
			return("avc_get: invalid decoded frame dimensions");
		}
		mp4_avc_csc_setup(&csc, info, yuv);
		csc_width = info->width > 480 ? 768 : 512;
		mp4_avc_cache_barrier("get", "before_csc", p, destination_buffer);
		call_result = sceMpegBaseCscAvc(destination_buffer, 0, csc_width, &csc);
		if (call_result != 0) {
			return("avc_get: sceMpegBaseCscAvc failed");
		}
		mp4_avc_cache_barrier("get", "after_csc", p, destination_buffer);
	}
	*pic_num = p->mpeg_pic_num;

	return(0);
}

char *mp4_avc_get_cache(struct mp4_avc_struct *p, void *destination_buffer, int pic_num) {

	Mp4AvcInfoStruct *info_buffer;
	Mp4AvcYuvStruct *yuv_buffer;
	Mp4AvcCscStruct csc;
	int csc_width;
	int offset;
	int call_result;

	if (p == 0 || destination_buffer == 0 || p->mpeg_detail2 == 0 ||
	    p->mpeg_detail2->info_buffer == 0 || p->mpeg_detail2->yuv_buffer == 0 ||
	    p->mpeg_pic_num <= 0 || pic_num <= 0 || pic_num > p->mpeg_pic_num) {
		return("avc_get_cache: invalid cached-frame request");
	}

	offset = p->mpeg_pic_num - pic_num;
	info_buffer = p->mpeg_detail2->info_buffer + offset;
	yuv_buffer = p->mpeg_detail2->yuv_buffer + offset;
	if (!PPA_VIDEO_DIMENSIONS_VALID(info_buffer->width, info_buffer->height)) {
		return("avc_get_cache: invalid cached-frame dimensions");
	}

	mp4_avc_cache_barrier("cache", "before_csc_setup", p, destination_buffer);
	mp4_avc_csc_setup(&csc, info_buffer, yuv_buffer);
	csc_width = info_buffer->width > 480 ? 768 : 512;
	mp4_avc_cache_barrier("cache", "before_csc", p, destination_buffer);
	call_result = sceMpegBaseCscAvc(destination_buffer, 0, csc_width, &csc);
	if (call_result != 0)
		return("avc_get_cache: sceMpegBaseCscAvc failed");
	mp4_avc_cache_barrier("cache", "after_csc", p, destination_buffer);
	return(0);
}
