#include "ppa_chapter.h"

int ppa_chapter_previous(const struct ppa_chapter *chapters,
                         unsigned int count, uint32_t current_ms)
{
    unsigned int i;
    int current = -1;
    if (!chapters || count == 0) return -1;
    for (i = 0; i < count; ++i) {
        if (chapters[i].time_ms > current_ms) break;
        current = (int)i;
    }
    if (current < 0) return 0;
    if (current_ms - chapters[current].time_ms <= 3000U && current > 0)
        return current - 1;
    return current;
}

int ppa_chapter_next(const struct ppa_chapter *chapters,
                     unsigned int count, uint32_t current_ms)
{
    unsigned int i;
    if (!chapters || count == 0) return -1;
    for (i = 0; i < count; ++i)
        if (chapters[i].time_ms > current_ms)
            return (int)i;
    return -1;
}
