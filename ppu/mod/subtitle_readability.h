#ifndef PPU_SUBTITLE_READABILITY_H
#define PPU_SUBTITLE_READABILITY_H
/* UTF-8 cue inspection, independent of track tags (also covers sidecars). */
int ppu_subtitle_needs_detail(const char *text);
int ppu_subtitle_readable_size(int base, int detailed);
#endif
