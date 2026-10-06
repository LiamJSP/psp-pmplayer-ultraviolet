#include "ppa_utf8.h"

#include <stdint.h>

static size_t emit_cp(char *dst, size_t cap, size_t out, uint32_t cp)
{
    if (cp <= 0x7fU) {
        if (out + 1U < cap) dst[out++] = (char)cp;
    } else if (cp <= 0x7ffU) {
        if (out + 2U < cap) {
            dst[out++] = (char)(0xc0U | (cp >> 6));
            dst[out++] = (char)(0x80U | (cp & 0x3fU));
        }
    } else if (cp <= 0xffffU) {
        if (out + 3U < cap) {
            dst[out++] = (char)(0xe0U | (cp >> 12));
            dst[out++] = (char)(0x80U | ((cp >> 6) & 0x3fU));
            dst[out++] = (char)(0x80U | (cp & 0x3fU));
        }
    } else if (cp <= 0x10ffffU && out + 4U < cap) {
        dst[out++] = (char)(0xf0U | (cp >> 18));
        dst[out++] = (char)(0x80U | ((cp >> 12) & 0x3fU));
        dst[out++] = (char)(0x80U | ((cp >> 6) & 0x3fU));
        dst[out++] = (char)(0x80U | (cp & 0x3fU));
    }
    return out;
}

size_t ppa_utf8_sanitize_copy(char *dst, size_t cap, const char *src)
{
    const unsigned char *p = (const unsigned char *)src;
    size_t out = 0;
    if (!dst || cap == 0) return 0;
    if (!p) { dst[0] = 0; return 0; }

    while (*p && out + 1U < cap) {
        uint32_t cp;
        unsigned int need;
        unsigned int i;
        unsigned char b = *p;
        int valid = 1;
        if (b < 0x80U) { cp = b; need = 1; }
        else if ((b & 0xe0U) == 0xc0U) { cp = b & 0x1fU; need = 2; }
        else if ((b & 0xf0U) == 0xe0U) { cp = b & 0x0fU; need = 3; }
        else if ((b & 0xf8U) == 0xf0U) { cp = b & 0x07U; need = 4; }
        else { valid = 0; cp = 0x25a1U; need = 1; }
        if (valid) {
            for (i = 1; i < need; ++i) {
                if (p[i] == 0 || (p[i] & 0xc0U) != 0x80U) { valid = 0; break; }
                cp = (cp << 6) | (p[i] & 0x3fU);
            }
            if ((need == 2 && cp < 0x80U) ||
                (need == 3 && cp < 0x800U) ||
                (need == 4 && cp < 0x10000U) ||
                cp > 0x10ffffU || (cp >= 0xd800U && cp <= 0xdfffU))
                valid = 0;
        }
        if (!valid) {
            cp = 0x25a1U;
            need = 1;
        }
        /* Keep line breaks/tabs, but never pass other control bytes to markup. */
        if ((cp < 0x20U && cp != '\n' && cp != '\t') || cp == 0x7fU)
            cp = 0x25a1U;
        out = emit_cp(dst, cap, out, cp);
        p += need;
    }
    dst[out] = 0;
    return out;
}
