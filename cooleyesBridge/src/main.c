#include <pspsdk.h>
#include <pspkernel.h>
#include <pspkerror.h>
#include <psploadcore.h>
#include <string.h>

#include "../cooleyesBridge.h"

#define VERS 3
#define REVS 1

PSP_MODULE_INFO("cooleyesBridge", 0x1006, VERS, REVS);
PSP_MAIN_THREAD_ATTR(0);

int sceAudioSetFrequency(int frequency);
int sceAudioSetFrequency371(int frequency);
int sceAudioSetFrequency380(int frequency);
int sceAudioSetFrequency395(int frequency);
int sceAudioSetFrequency500(int frequency);
int sceAudioSetFrequency620(int frequency);
int sceAudioSetFrequency635(int frequency);
int sceAudioSetFrequency660(int frequency);

int sceMeBootStart(int mebooterType);
int sceMeBootStart371(int mebooterType);
int sceMeBootStart380(int mebooterType);
int sceMeBootStart395(int mebooterType);
int sceMeBootStart500(int mebooterType);
int sceMeBootStart620(int mebooterType);
int sceMeBootStart635(int mebooterType);
int sceMeBootStart660(int mebooterType);

/* PSPSDK sceDisplay_driver imports are resolved/remapped by supported CFW. */
int sceDisplayEnable(void);
int sceDisplayDisable(void);
int sceDisplayGetBrightness(int *level, int *unknown);
int sceDisplaySetBrightness(int level, int unknown);
static int g_display_disabled;
static int g_saved_brightness = 68;
static int g_devkit_version;

static int bridge_devkit_version(int requested)
{
    if (requested > 0)
        return requested;
    if (g_devkit_version <= 0)
        g_devkit_version = sceKernelDevkitVersion();
    return g_devkit_version;
}

static int bridge_audio_set_frequency_for_fw(int devkit, int frequency)
{
    if (devkit < 0x03070000)
        return sceAudioSetFrequency(frequency);
    if (devkit < 0x03080000)
        return sceAudioSetFrequency371(frequency);
    if (devkit < 0x03090500)
        return sceAudioSetFrequency380(frequency);
    if (devkit < 0x05000000)
        return sceAudioSetFrequency395(frequency);
    if (devkit < 0x06020000)
        return sceAudioSetFrequency500(frequency);
    if (devkit < 0x06030500)
        return sceAudioSetFrequency620(frequency);
    if (devkit < 0x06060000)
        return sceAudioSetFrequency635(frequency);
    return sceAudioSetFrequency660(frequency);
}

static int bridge_me_boot_start_for_fw(int devkit, int mebooter_type)
{
    if (devkit < 0x03070000)
        return sceMeBootStart(mebooter_type);
    if (devkit < 0x03080000)
        return sceMeBootStart371(mebooter_type);
    if (devkit < 0x03090500)
        return sceMeBootStart380(mebooter_type);
    if (devkit < 0x05000000)
        return sceMeBootStart395(mebooter_type);
    if (devkit < 0x06020000)
        return sceMeBootStart500(mebooter_type);
    if (devkit < 0x06030500)
        return sceMeBootStart620(mebooter_type);
    if (devkit < 0x06060000)
        return sceMeBootStart635(mebooter_type);
    return sceMeBootStart660(mebooter_type);
}

unsigned int cooleyesBridgeGetVersion(void)
{
    return COOLEYES_BRIDGE_API_VERSION;
}

unsigned int cooleyesBridgeGetCapabilities(void)
{
    return COOLEYES_BRIDGE_CAP_AUDIO_FREQUENCY |
           COOLEYES_BRIDGE_CAP_ME_BOOT |
           COOLEYES_BRIDGE_CAP_DISPLAY_POWER |
           COOLEYES_BRIDGE_CAP_REGION_CODE;
}

int cooleyesAudioSetFrequency(int devkitVersion, int frequency)
{
    u32 old_k1;
    int result;

    if (frequency != 44100 && frequency != 48000)
        return SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;

    old_k1 = pspSdkSetK1(0);
    result = bridge_audio_set_frequency_for_fw(
        bridge_devkit_version(devkitVersion), frequency);
    pspSdkSetK1(old_k1);
    return result;
}

int cooleyesMeBootStart(int devkitVersion, int mebooterType)
{
    u32 old_k1;
    int result;

    if (mebooterType < 0)
        return SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;

    old_k1 = pspSdkSetK1(0);
    result = bridge_me_boot_start_for_fw(
        bridge_devkit_version(devkitVersion), mebooterType);
    pspSdkSetK1(old_k1);
    return result;
}

int cooleyesDisplaySetEnabled(int enabled)
{
    u32 old_k1;
    int result, brightness, brightness_unknown;
    if (enabled != 0 && enabled != 1)
        return SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
    old_k1 = pspSdkSetK1(0);
    if (enabled) {
        result = sceDisplayEnable();
        if (g_display_disabled) {
            int restore = sceDisplaySetBrightness(g_saved_brightness, 0);
            if (result >= 0) result = restore;
        }
        if (result >= 0) g_display_disabled = 0;
    } else if (g_display_disabled) {
        result = 0;
    } else {
        brightness = 0;
        brightness_unknown = 0;
        result = sceDisplayGetBrightness(&brightness, &brightness_unknown);
        if (result >= 0 && brightness > 0) g_saved_brightness = brightness;
        result = sceDisplaySetBrightness(0, 0);
        if (result >= 0) {
            result = sceDisplayDisable();
            if (result >= 0) g_display_disabled = 1;
            else (void)sceDisplaySetBrightness(g_saved_brightness, 0);
        }
    }
    pspSdkSetK1(old_k1);
    return result;
}

int module_start(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    g_devkit_version = sceKernelDevkitVersion();
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    if (g_display_disabled) (void)cooleyesDisplaySetEnabled(1);
    g_devkit_version = 0;
    return 0;
}

/* Resolve this optional service without a mandatory chkreg import. Missing
 * modules/exports must degrade to International, not prevent PPU booting.
 * Only a module loaded here is stopped/unloaded here. All buffers are local. */
static int (*bridge_find_pscode(SceModule *module))(unsigned char *)
{
    unsigned int offset = 0;
    if (!module || !module->ent_top || module->ent_size > 65536U) return 0;
    while (offset + sizeof(SceLibraryEntryTable) <= module->ent_size) {
        SceLibraryEntryTable *entry = (SceLibraryEntryTable *)
            ((unsigned char *)module->ent_top + offset);
        unsigned int size = (unsigned int)entry->len * 4U;
        unsigned int i, total = entry->stubcount + entry->vstubcount;
        unsigned int *table = (unsigned int *)entry->entrytable;
        if (size < sizeof(SceLibraryEntryTable) || size > module->ent_size - offset)
            return 0;
        if (entry->libname && table && total <= 1024U &&
            (!strcmp(entry->libname, "sceChkreg") ||
             !strcmp(entry->libname, "sceChkreg_driver"))) {
            for (i = 0; i < entry->stubcount; ++i)
                if (table[i] == 0x59F8491DU && table[total + i])
                    return (int (*)(unsigned char *))table[total + i];
        }
        offset += size;
    }
    return 0;
}

int cooleyesGetRegionCode(void)
{
    unsigned char code[8] = {0};
    u32 old_k1 = pspSdkSetK1(0);
    SceUID owned = -1;
    SceModule *module = sceKernelFindModuleByName("sceChkreg");
    int (*get_pscode)(unsigned char *) = 0;
    int status = 0, result = -1;
    if (!module) {
        owned = sceKernelLoadModule("flash0:/kd/chkreg.prx", 0, 0);
        if (owned >= 0) {
            if (sceKernelStartModule(owned, 0, 0, &status, 0) < 0 || status < 0) {
                sceKernelUnloadModule(owned);
                owned = -1;
            } else module = sceKernelFindModuleByName("sceChkreg");
        }
    }
    get_pscode = bridge_find_pscode(module);
    if (get_pscode && get_pscode(code) == 0 &&
        code[0] == 1 && code[1] == 0 && code[3] == 0 &&
        code[4] == 1 && code[5] == 0 && code[6] == 1 && code[7] == 0) {
        switch (code[2]) {
        case 3: case 4: case 5: case 6: case 7: case 8:
        case 9: case 10: case 11: case 12: case 13:
            result = code[2];
            break;
        default: break;
        }
    }
    if (owned >= 0 && sceKernelStopModule(owned, 0, 0, &status, 0) >= 0)
        sceKernelUnloadModule(owned);
    pspSdkSetK1(old_k1);
    return result;
}
