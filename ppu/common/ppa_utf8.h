#ifndef PPA_UTF8_H
#define PPA_UTF8_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copies valid UTF-8 and replaces malformed byte sequences, embedded controls,
 * and unsupported surrogate code points with U+25A1 (WHITE SQUARE). */
size_t ppa_utf8_sanitize_copy(char *dst, size_t capacity, const char *src);

#ifdef __cplusplus
}
#endif

#endif
