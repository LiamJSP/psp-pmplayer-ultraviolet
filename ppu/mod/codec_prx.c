#include <psputility_avmodules.h>
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
 
#include "codec_prx.h"
#include "common/m33sdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
 
/* Track only modules acquired by this application. Dependency order is
 * MPEG-VSH -> AVCODEC on teardown; an unsuccessful stop retains ownership. */
#include "mp4avcdecoder.h"
#include "audiodecoder.h"
#include "common/ppa_privileged_bridge.h"

static SceUID g_mpeg_module = -1;
static int g_mpeg_started;
static int g_avcodec_owned;

void media_codecs_close(struct mp4_avc_struct *avc, int *audio_state)
{
    mp4_avc_close(avc);
    if (audio_state != 0 && *audio_state == 0) {
        audio_decoder_close();
        *audio_state = -1;
    }
    /* An AVC-only seek intentionally does not pass here. Boot-mode caching
     * must not survive the end of a complete video/audio firmware session. */
    if (!mp4_avc_is_active() && !audio_decoder_is_open())
        ppa_privileged_me_boot_invalidate();
}

int unload_codec_prx(void)
{
    int result, status = 0;
    if (mp4_avc_is_active() || audio_decoder_is_open())
        return -1;
    if (g_mpeg_module >= 0) {
        if (g_mpeg_started) {
            result = sceKernelStopModule(g_mpeg_module, 0, 0, &status, 0);
            if (result < 0) return result;
            if (status < 0) return status;
            g_mpeg_started = 0;
        }
        result = sceKernelUnloadModule(g_mpeg_module);
        if (result < 0) return result;
        g_mpeg_module = -1;
    }
    if (g_avcodec_owned) {
        result = sceUtilityUnloadAvModule(PSP_AV_MODULE_AVCODEC);
        if (result < 0) return result;
        g_avcodec_owned = 0;
    }
    ppa_privileged_me_boot_invalidate();
    return 0;
}

char *load_codec_prx(const char* ppa_path, int devkitVersion)
{
    char prx_path[512];
    const char *name;
    int result, status = 0;

    if (g_mpeg_module >= 0 && g_mpeg_started && g_avcodec_owned)
        return 0;
    if ((g_mpeg_module >= 0 || g_avcodec_owned) && unload_codec_prx() < 0)
        return "codec_prx: previous module cleanup failed";
    if (ppa_path == 0) return "codec_prx: invalid application path";
    name = devkitVersion < 0x03050000 ? "mpeg_vsh330.prx" :
           devkitVersion < 0x03070000 ? "mpeg_vsh350.prx" :
                                      "mpeg_vsh370.prx";
    result = snprintf(prx_path, sizeof(prx_path), "%s%s",
                      devkitVersion < 0x05000000 ? ppa_path : "flash0:/kd/",
                      devkitVersion < 0x05000000 ? name : "mpeg_vsh.prx");
    if (result < 0 || result >= (int)sizeof(prx_path))
        return "codec_prx: module path too long";

    result = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
    if (result < 0)
        return "codec_prx: could not load avcodec.prx";
    g_avcodec_owned = 1;
    g_mpeg_module = m33KernelLoadModule(prx_path, 0, NULL);
    if (g_mpeg_module < 0) {
        (void)unload_codec_prx();
        return "codec_prx: could not load mpeg_vsh.prx";
    }
    result = sceKernelStartModule(g_mpeg_module, 0, 0, &status, NULL);
    if (result < 0 || status < 0) {
        /* A negative start/module result did not establish a running module.
         * Unload this owned image; never unload AVCODEC if that release fails. */
        (void)unload_codec_prx();
        return "codec_prx: could not start mpeg_vsh.prx";
    }
    g_mpeg_started = 1;
    return 0;
}
