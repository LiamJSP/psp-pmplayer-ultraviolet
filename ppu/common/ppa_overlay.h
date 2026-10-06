#ifndef PPA_OVERLAY_H
#define PPA_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPA_OVERLAY_MAX_MESSAGES 6
#define PPA_OVERLAY_TEXT_CAPACITY 384

enum ppa_overlay_key {
    PPA_OVERLAY_KEY_INFO = 1,
    PPA_OVERLAY_KEY_SEEK = 2,
    PPA_OVERLAY_KEY_CHAPTER = 3,
    PPA_OVERLAY_KEY_SUBTITLE = 4
};

enum ppa_overlay_position {
    PPA_OVERLAY_TOP_RIGHT = 0,
    PPA_OVERLAY_CENTER = 1,
    PPA_OVERLAY_BOTTOM_CENTER = 2,
    PPA_OVERLAY_TOP_LEFT = 3
};

enum ppa_overlay_flags {
    PPA_OVERLAY_UTF8 = 1U << 0,
    PPA_OVERLAY_WHITE = 1U << 1
};

struct ppa_overlay_message {
    unsigned int key;
    unsigned int position;
    unsigned int flags;
    uint64_t expires_us;
    char text[PPA_OVERLAY_TEXT_CAPACITY];
};

void ppa_overlay_reset(void);
void ppa_overlay_post(unsigned int key, unsigned int position,
                      unsigned int duration_ms, unsigned int flags,
                      const char *text);
void ppa_overlay_postf(unsigned int key, unsigned int position,
                       unsigned int duration_ms, unsigned int flags,
                       const char *format, ...);
void ppa_overlay_clear(unsigned int key);
unsigned int ppa_overlay_snapshot(struct ppa_overlay_message *out,
                                  unsigned int capacity,
                                  uint64_t now_us);
int ppa_overlay_has_active(uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif
