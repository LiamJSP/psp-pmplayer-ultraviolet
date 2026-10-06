#ifndef PPA_CHAPTER_H
#define PPA_CHAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPA_MAX_CHAPTERS 128
#define PPA_CHAPTER_TITLE_CAPACITY 192

struct ppa_chapter {
    uint32_t time_ms;
    char title[PPA_CHAPTER_TITLE_CAPACITY];
};

/* Returns a zero-based chapter index, or -1 when no target exists. Previous
 * follows common player behavior: within three seconds of a chapter boundary
 * it goes to the preceding chapter; otherwise it restarts the current one. */
int ppa_chapter_previous(const struct ppa_chapter *chapters,
                         unsigned int count, uint32_t current_ms);
int ppa_chapter_next(const struct ppa_chapter *chapters,
                     unsigned int count, uint32_t current_ms);

#ifdef __cplusplus
}
#endif

#endif
