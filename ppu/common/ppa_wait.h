#ifndef PPA_WAIT_H
#define PPA_WAIT_H

#include <pspkernel.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPA_SEMA_WAIT_TIMEOUT_US 8000U
/* AV-sync retry delay is intentionally short; it is not an idle UI wait. */
#define PPA_SEMA_IDLE_DELAY_US   1000U

int ppa_wait_sema_poll(volatile int *return_request,
                       SceUID sema,
                       const char *error_message,
                       char **return_result,
                       unsigned timeout_us);

/* Playback queues use bounded waits. Stop their owner before joining; never
 * signal synthetic queue credits to wake workers during shutdown. */

/* Normal waits are bounded; a slow owner then remains joinable in one-second
 * slices. Only a confirmed stopped (not killed) thread permits teardown.
 * Never interpret a timeout as permission to free firmware-visible storage. */
int ppa_wait_thread_end_safe(SceUID thread_id,
                                 const char *subsystem,
                                 const char *name,
                                 unsigned int wait_us,
                                 unsigned int grace_wait_us);

/* Does not return. Reserved for an invalid/killed owner or broken join contract,
 * not ordinary wait timeouts. Retains hardware-visible state on an unproven stop. */
void ppa_wait_quarantine(const char *reason, int error) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif
