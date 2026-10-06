#ifndef PPA_CTRL_H
#define PPA_CTRL_H

#include <pspctrl.h>
#include <psphprm.h>
#include <psptypes.h>

#define CTRL_FORWARD   0x10000000U
#define CTRL_BACK      0x20000000U
#define CTRL_PLAYPAUSE 0x40000000U
#define CTRL_ANALOG    0x80000000U

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Controller service.
 *
 * A dedicated low-cost sampling thread drains the PSP controller buffer and
 * records release/chord events in a software ring. This prevents short presses
 * from disappearing while the UI performs I/O or redraws. ctrl_wait() sleeps
 * on a kernel event flag and is the preferred UI-loop API.
 */
enum ctrl_context {
    CTRL_CONTEXT_BROWSER, CTRL_CONTEXT_MENU,
    CTRL_CONTEXT_PLAYBACK, CTRL_CONTEXT_AUDIO_ONLY
};
/* Context changes discard queued intent and consume held buttons until release.
 * A chord fires once when its last button goes down, in either order. Each
 * participant's later release is consumed; unchorded singles fire on up,
 * except playback left/right: down once, then four held ticks per second;
 * bare browser up/down: down once, repeat after 350 ms at 80 ms intervals. */
enum ctrl_context ctrl_set_context(enum ctrl_context context);
int  ctrl_init(void);
void ctrl_destroy(void);
void ctrl_analog(int *x, int *y);
u32  ctrl_read_cont(void);
u32  ctrl_read(void);
int  ctrl_peek_sample(SceCtrlData *sample);
/* Event API: Buttons is zero when there is no event. Never a held snapshot. */
int  ctrl_read_sample(SceCtrlData *sample);
int  ctrl_read_sample_wait(SceCtrlData *sample, unsigned int timeout_us);
u32  ctrl_wait(unsigned int timeout_us);
void ctrl_flush(void);
int  ctrl_pending(void);
/* Time since the sampler last saw pressed/held/released input, using the
 * kernel microsecond clock. Does not consume events or wait on the queue. */
unsigned int ctrl_idle_time_us(void);
u32  ctrl_hprm(void);
void ctrl_enablehprm(int enable);

#ifdef __cplusplus
}
#endif

#endif
