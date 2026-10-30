#include "ppa_debug_state.h"
#include <stddef.h>
static uint32_t checksum(const struct ppa_debug_state *s)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t hash = 2166136261U;
    unsigned int i;
    for (i = 0; i < offsetof(struct ppa_debug_state, checksum); ++i)
        hash = (hash ^ p[i]) * 16777619U;
    return hash;
}
void ppa_debug_state_seal(struct ppa_debug_state *s)
{
    s->magic = 0x50505544U;
    s->version = 1U;
    s->checksum = checksum(s);
}
int ppa_debug_state_valid(const struct ppa_debug_state *s)
{
    return s->magic == 0x50505544U && s->version == 1U && s->enabled <= 1U &&
           s->remaining <= 3U && s->pending <= 1U &&
           s->checksum == checksum(s);
}
int ppa_debug_state_newer(const struct ppa_debug_state *a, const struct ppa_debug_state *b)
{
    return (int32_t)(a->generation - b->generation) > 0;
}
