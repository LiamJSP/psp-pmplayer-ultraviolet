#ifndef PPA_DEBUG_STATE_H
#define PPA_DEBUG_STATE_H
#include <stdint.h>
/* Two alternating, checksummed records. A start reserves one playback before
 * workers run, so crashes/power loss cannot silently reset the allowance. */
struct ppa_debug_state {
    uint32_t magic, version, generation, enabled, remaining, pending, checksum;
};
void ppa_debug_state_seal(struct ppa_debug_state *s);
int ppa_debug_state_valid(const struct ppa_debug_state *s);
int ppa_debug_state_newer(const struct ppa_debug_state *a, const struct ppa_debug_state *b);
#endif
