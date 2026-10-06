/*
 * Small CFW compatibility wrapper used by the legacy PPA call sites.
 * Privileged module/model operations are provided by KUBridge; this file does
 * not duplicate cooleyesBridge's audio/Media Engine exports.
 */
#include "m33sdk.h"

#include <kubridge.h>
#include <pspsdk.h>

int m33KernelGetModel(void)
{
    return kuKernelGetModel();
}

int m33IsTVOutSupported(int model_generation)
{
    int model_has_tvout;

    switch (model_generation) {
    case 1: /* 02g PSP-2000 */
    case 2: /* 03g PSP-3000 */
    case 3: /* 04g PSP-3000 */
    case 4: /* 05g PSP Go */
    case 6: /* 07g PSP-3000 */
    case 7: /* 09g PSP-3000 */
    case 8: /* 08g PSP-3000 */
        model_has_tvout = 1;
        break;
    default: /* 01g Fat, 11g Street, unknown */
        model_has_tvout = 0;
        break;
    }

    return model_has_tvout && sceKernelDevkitVersion() >= 0x03070110;
}

SceUID m33KernelLoadModule(const char *path,
                           int flags,
                           SceKernelLMOption *option)
{
    return kuKernelLoadModule(path, flags, option);
}
