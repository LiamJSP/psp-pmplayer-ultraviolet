#include "ppa_privileged_bridge.h"

#include <pspkernel.h>
#include "cooleyesBridge.h"
#include "../mod/mp4avcdecoder.h"
#include "../mod/audiodecoder.h"

static unsigned int g_bridge_version;
static unsigned int g_bridge_capabilities;
static int g_bridge_attached;
static int g_audio_frequency;
static int g_mebooter_type = -1;

int ppa_privileged_bridge_attach(void)
{
    const unsigned int required = COOLEYES_BRIDGE_CAP_AUDIO_FREQUENCY |
                                  COOLEYES_BRIDGE_CAP_ME_BOOT;
    g_bridge_version = cooleyesBridgeGetVersion();
    g_bridge_capabilities = cooleyesBridgeGetCapabilities();
    g_bridge_attached = (g_bridge_version >= COOLEYES_BRIDGE_API_VERSION &&
                         (g_bridge_capabilities & required) == required);
    g_audio_frequency = 0;
    g_mebooter_type = -1;
    return g_bridge_attached;
}

void ppa_privileged_bridge_reset(void)
{
    g_bridge_version = 0;
    g_bridge_capabilities = 0;
    g_bridge_attached = 0;
    g_audio_frequency = 0;
    g_mebooter_type = -1;
}

unsigned int ppa_privileged_bridge_version(void) { return g_bridge_version; }
unsigned int ppa_privileged_bridge_capabilities(void) { return g_bridge_capabilities; }

int ppa_privileged_display_set_enabled(int enabled)
{
    if (!g_bridge_attached ||
        !(g_bridge_capabilities & COOLEYES_BRIDGE_CAP_DISPLAY_POWER)) return -1;
    return cooleyesDisplaySetEnabled(enabled);
}

int ppa_privileged_audio_set_frequency(int frequency)
{
    int result;
    if (!g_bridge_attached ||
        (g_bridge_capabilities & COOLEYES_BRIDGE_CAP_AUDIO_FREQUENCY) == 0)
        return -1;
    if (frequency != 44100 && frequency != 48000)
        return -1;
    if (frequency == g_audio_frequency) {
        return 0;
    }
    result = cooleyesAudioSetFrequency(0, frequency);
    if (result == 0)
        g_audio_frequency = frequency;
    return result;
}

void ppa_privileged_me_boot_invalidate(void)
{
    g_mebooter_type = -1;
}

int ppa_privileged_me_boot_start(int mebooter_type)
{
    int result;
    if (!g_bridge_attached ||
        (g_bridge_capabilities & COOLEYES_BRIDGE_CAP_ME_BOOT) == 0)
        return -1;
    if (mebooter_type < 0)
        return -1;
    if (mebooter_type == g_mebooter_type) {
        return 0;
    }
    /* A different firmware image must never replace a live AVC/audio codec.
     * Same-mode AVC-only seeks reuse the boot above. A compatibility fallback
     * requesting a different image after audio initialization fails through
     * normal session cleanup instead of invalidating that audio workspace. */
    if (mp4_avc_is_active() || audio_decoder_is_open())
        return -1;
    /* A failed attempt to switch modes cannot validate the previous cache. */
    g_mebooter_type = -1;
    result = 
        cooleyesMeBootStart(0, mebooter_type);
    if (result == 0)
        g_mebooter_type = mebooter_type;
    return result;
}

int ppa_privileged_region_code(void)
{
    if (!g_bridge_attached ||
        !(g_bridge_capabilities & COOLEYES_BRIDGE_CAP_REGION_CODE)) return -1;
    return cooleyesGetRegionCode();
}
