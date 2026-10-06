#ifndef subtitle_font_config_h__
#define subtitle_font_config_h__

#include "mkvinfo_type.h"

#ifdef __cplusplus
extern "C" {
#endif

void subtitle_font_config_set_directory(const char *directory);
void subtitle_font_config_load_for_track(const mkvinfo_track_t *track);
void subtitle_font_config_load_core(void);
void subtitle_font_config_clear(void);

#ifdef __cplusplus
}
#endif

#endif