#ifndef PPA_PRIVILEGED_BRIDGE_H
#define PPA_PRIVILEGED_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* User-side policy and telemetry wrapper for the narrow cooleyesBridge PRX.
 * The PRX remains a compatibility bridge to two firmware driver calls; this
 * layer prevents redundant kernel transitions and centralizes ABI validation. */
int ppa_privileged_bridge_attach(void);
int ppa_privileged_region_code(void);
void ppa_privileged_bridge_reset(void);
unsigned int ppa_privileged_bridge_version(void);
unsigned int ppa_privileged_bridge_capabilities(void);
int ppa_privileged_display_set_enabled(int enabled);
int ppa_privileged_audio_set_frequency(int frequency);
int ppa_privileged_me_boot_start(int mebooter_type);
void ppa_privileged_me_boot_invalidate(void);
/* Invalidation only clears our cached assumption; it does not reset hardware.
 * Full-session close calls it after both codecs stop. Do not invalidate on an
 * ordinary AVC-only seek/retry while the audio codec is still initialized. */

#ifdef __cplusplus
}
#endif

#endif
