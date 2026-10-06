#ifndef PPU_CONTROLS_H
#define PPU_CONTROLS_H
#include <pspctrl.h>
#ifdef __cplusplus
extern "C" {
#endif
enum ppu_controls_type { PPU_CONTROLS_INTERNATIONAL, PPU_CONTROLS_ASIA };
/* Returns true only when a detected/default value was added to config. */
int ppu_controls_init(void);
int ppu_controls_get_type(void);
void ppu_controls_set_type(int type);
unsigned int ppu_controls_confirm(void);
unsigned int ppu_controls_cancel(void);
const char *ppu_controls_confirm_glyph(void);
const char *ppu_controls_cancel_glyph(void);
#ifdef __cplusplus
}
#endif
#endif
