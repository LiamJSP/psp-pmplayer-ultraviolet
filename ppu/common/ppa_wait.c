#include "ppa_wait.h"

#include <pspthreadman.h>
#include <pspdebug.h>
#include <stdio.h>
#include <string.h>

int ppa_wait_sema_poll(volatile int *return_request,
                           SceUID sema,
                           const char *error_message,
                           char **return_result,
                           unsigned timeout_us) {
	SceUInt timeout = timeout_us;
	int result;

	if (return_request != 0 && *return_request != 0)
		return 0;
	result = sceKernelWaitSema(sema, 1, &timeout);

	/* Stop can arrive during the bounded wait. A credit acquired now belongs
	 * to the terminating queue: do not start more work or report a stop as a
	 * playback error. Queue semaphores are deleted after all workers join. */
	if (return_request != 0 && *return_request != 0)
		return 0;

	if (result == SCE_KERNEL_ERROR_OK) {
		return 1;
	}

	if (result == SCE_KERNEL_ERROR_WAIT_TIMEOUT) {

		return 0;
	}

	if (return_result != 0)
		*return_result = (char *)error_message;

	if (return_request != 0)
		*return_request = 1;

	return -1;
}

int ppa_wait_thread_end_safe(SceUID thread_id,
                                 const char *subsystem,
                                 const char *name,
                                 unsigned int wait_us,
                                 unsigned int grace_wait_us)
{
    SceUInt timeout;
    int result;
    unsigned int phase = 0;
    (void)subsystem;
    (void)name;
    if (thread_id < 0) return 0;
    for (;;) {
        SceKernelThreadInfo info;
        int status_result;
        timeout = phase == 0U ? wait_us :
                  phase == 1U && grace_wait_us != 0U ? grace_wait_us : 1000000U;
        if (timeout == 0U) timeout = 1000000U;
        result = sceKernelWaitThreadEnd(thread_id, &timeout);
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        status_result = sceKernelReferThreadStatus(thread_id, &info);
        if (status_result < 0) return status_result;
        /* A fault/stack overflow is not a cooperative firmware handoff. */
        if (info.status & PSP_THREAD_KILLED) return -1;
        if (info.status & PSP_THREAD_STOPPED) return 0;
        if (result != SCE_KERNEL_ERROR_WAIT_TIMEOUT)
            return result < 0 ? result : -1;
        /* Keep the owner, its stack and all device buffers alive. Previously
         * one late return led to permanent quarantine even if the call ended
         * a moment later. No reset, second codec call or forced exit occurs
         * while waiting; normal cleanup resumes as soon as the owner stops. */
        if (phase < 2U) ++phase;
    }
}

void ppa_wait_quarantine(const char *reason, int error)
{
    static volatile unsigned int entered;
    /* A decoder worker and the controller may both discover the same stall.
     * Only the first writes the report; neither may unwind its retained stack. */
    if (__sync_bool_compare_and_swap(&entered, 0U, 1U)) {
        pspDebugScreenInit();
        pspDebugScreenPrintf("Playback could not stop safely.\n%s\nError: 0x%08x\n"
                            "Power off the PSP before trying again.\n"
                            "HOME cannot safely release this session.\n",
                            reason ? reason : "Firmware did not return",
                            (unsigned)error);
    }
    for (;;) sceKernelSleepThread();
}
