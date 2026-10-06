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

#include <stdio.h>
#include <pspkernel.h>
#include <pspsdk.h>
#include <string.h>
#include "videomode.h"
#include "common/m33sdk.h"
#include "common/ppa_memory.h"
#include "common/ppa_video_output.h"

#ifdef __cplusplus
extern "C" {
#endif

int pspDveMgrCheckVideoOut();
int pspDveMgrSetVideoOut(int u, int mode, int width, int height, int x, int y, int z);

#ifdef __cplusplus
}
#endif

int VideoMode::videoMode = 0; //0:PSP, 1:composite, 2:component interlace, 3:component progressive
int VideoMode::tvAspectRatio = 0; //0=16:9, 1=4:3
int VideoMode::tvOverScanLeft = 8;
int VideoMode::tvOverScanTop = 0;
int VideoMode::tvOverScanRight = 8;
int VideoMode::tvOverScanBottom = 0;
int VideoMode::pspType = 0;
bool VideoMode::driverReady = false;

void VideoMode::setTVOverScan(int left, int top, int right, int bottom) {
	/* Config is untrusted. Keep a nonempty viewport before unsigned GE math. */
	tvOverScanLeft = ppa_video_output_margin(left, 320);
	tvOverScanRight = ppa_video_output_margin(right, 320);
	tvOverScanTop = ppa_video_output_margin(top, 224);
	tvOverScanBottom = ppa_video_output_margin(bottom, 224);
}

bool VideoMode::init(int type, const char* prx) {
	VideoMode::pspType = type;
	VideoMode::driverReady = false;
	if ( m33IsTVOutSupported(type) ) {
		SceUID modid;
		int status;
		modid = m33KernelLoadModule(prx, 0, NULL);
		if (modid < 0){
			return false;
		}
		int started = sceKernelStartModule(modid, 0, 0, &status, NULL);
		if (started < 0 || status < 0){
			sceKernelUnloadModule(modid);
			return false;
		}
		else {
			VideoMode::driverReady = true;
			return true;
		}
	}
	else
		return true;
};

int VideoMode::setVideoMode(int mode) {
	if (mode < 0 || mode > 3 || !VideoMode::driverReady ||
	    (mode != 0 && !ppa_memory_fixed_region_reserved()))
		return VideoMode::videoMode;
	if ( m33IsTVOutSupported(VideoMode::pspType) && (mode != VideoMode::videoMode) ) {
		if (mode == 0) {
			if (pspDveMgrSetVideoOut(0, 0, 480, 272, 1, 15, 0) >= 0)
				VideoMode::videoMode = mode;
		}
		else {
			int cable = pspDveMgrCheckVideoOut();
			if ( (cable == 1 && mode == 1) || (cable == 2 && mode == 2) || (cable == 2 && mode == 3) ) {
				int result;
				if ( cable == 1 && mode == 1) {
					result = pspDveMgrSetVideoOut(2, 0x1d1, 720, 503, 1, 15, 0);
				}
				else if ( cable == 2 && mode == 2) {
					result = pspDveMgrSetVideoOut(0, 0x1d1, 720, 503, 1, 15, 0);
				}
				else {
					result = pspDveMgrSetVideoOut(0, 0x1d2, 720, 480, 1, 15, 0);
				}
				if (result >= 0)
					VideoMode::videoMode = mode;
			}
		}
		return VideoMode::videoMode;
	}
	else
		return VideoMode::videoMode;
};
