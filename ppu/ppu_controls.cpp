#include "common/ppu_controls.h"
#include "common/ppa_privileged_bridge.h"
#include "config.h"
#include <strings.h>

static int controls_type = PPU_CONTROLS_INTERNATIONAL;
int ppu_controls_init(void)
{
    Config *config = Config::getInstance();
    const char *saved = config ?
        config->getStringValue("config/player/controls_type", "") : "";
    if (saved && strcasecmp(saved, "International") == 0)
        controls_type = PPU_CONTROLS_INTERNATIONAL;
    else if (saved && strcasecmp(saved, "Asia") == 0)
        controls_type = PPU_CONTROLS_ASIA;
    else {
        /* Physical region, not UI language or a CFW-swapped XMB preference.
         * Missing firmware service, errors and unknown codes all choose X. */
        int region = ppa_privileged_region_code();
        switch (region) {
        case 3: case 6: case 10: case 11: case 13:
            controls_type = PPU_CONTROLS_ASIA;
            break; /* Japan, Korea, Hong Kong, Taiwan, China */
        default:
            controls_type = PPU_CONTROLS_INTERNATIONAL;
            break;
        }
        if (config)
            return config->setStringValue("config/player/controls_type",
                controls_type == PPU_CONTROLS_ASIA ? "Asia" : "International");
    }
    return 0;
}
int ppu_controls_get_type(void) { return controls_type; }
void ppu_controls_set_type(int type)
{ controls_type = type == PPU_CONTROLS_ASIA ? PPU_CONTROLS_ASIA : PPU_CONTROLS_INTERNATIONAL; }
unsigned int ppu_controls_confirm(void)
{ return controls_type == PPU_CONTROLS_ASIA ? PSP_CTRL_CIRCLE : PSP_CTRL_CROSS; }
unsigned int ppu_controls_cancel(void)
{ return controls_type == PPU_CONTROLS_ASIA ? PSP_CTRL_CROSS : PSP_CTRL_CIRCLE; }
const char *ppu_controls_confirm_glyph(void)
{ return controls_type == PPU_CONTROLS_ASIA ? "\xE2\x97\x8B" : "\xC3\x97"; }
const char *ppu_controls_cancel_glyph(void)
{ return controls_type == PPU_CONTROLS_ASIA ? "\xC3\x97" : "\xE2\x97\x8B"; }
