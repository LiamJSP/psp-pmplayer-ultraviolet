#include "subtitle_readability.h"

int ppu_subtitle_needs_detail(const char *text)
{
    const unsigned char *s = (const unsigned char *)text;
    if (!s) return 0;
    while (*s) {
        unsigned int c = *s++, n = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { c &= 31; n = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 15; n = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { c &= 7; n = 3; minimum = 0x10000; }
        else if (c >= 0x80) continue;
        while (n && (*s & 0xc0) == 0x80) { c = (c << 6) | (*s++ & 63); --n; }
        if (n || c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) continue;
        /* Hebrew, Arabic (incl. Persian/Urdu), Indic, Thai/Lao, Tibetan,
         * Myanmar, Khmer, CJK, Kana, Hangul and supplementary ideographs.
         * Latin, Greek and Cyrillic retain the configured metrics. */
        if ((c >= 0x0590 && c <= 0x109f) ||
            (c >= 0x1100 && c <= 0x11ff) ||
            (c >= 0x1780 && c <= 0x17ff) ||
            (c >= 0x1900 && c <= 0x1cff) ||
            (c >= 0x2e80 && c <= 0xa4cf) ||
            (c >= 0xa800 && c <= 0xaaff) ||
            (c >= 0xac00 && c <= 0xd7af) ||
            (c >= 0xf900 && c <= 0xfaff) ||
            (c >= 0xfb1d && c <= 0xfdff) ||
            (c >= 0xfe70 && c <= 0xfeff) ||
            (c >= 0xff66 && c <= 0xff9f) ||
            (c >= 0x11000 && c <= 0x11fff) ||
            (c >= 0x1b000 && c <= 0x1b2ff) ||
            (c >= 0x20000 && c <= 0x323af)) return 1;
    }
    return 0;
}

int ppu_subtitle_readable_size(int base, int detailed)
{
    /* Preserve every value offered by the existing 1..48 subtitle menu. */
    if (base <= 0 || base > 48) base = 16;
    return detailed && base < 20 ? 20 : base;
}
