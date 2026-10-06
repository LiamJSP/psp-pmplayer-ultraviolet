#include "ctrl.h"
#include "ppa_thread_policy.h"

#include <pspkernel.h>
#include <pspthreadman.h>
#include <string.h>

#ifndef PSP_THREAD_ATTR_USER
#define PSP_THREAD_ATTR_USER 0x80000000U
#endif

#define CTRL_QUEUE_CAPACITY 64U
#define CTRL_EVENT_AVAILABLE 0x00000001U
#define CTRL_THREAD_STACK_SIZE 0x1000
#define CTRL_SEEK_REPEAT_US 250000U
#define CTRL_BROWSER_REPEAT_DELAY_US 350000U
#define CTRL_BROWSER_REPEAT_US 80000U

/* The overflow latch retains command identity. OR-ing singles/chords together
 * would fabricate a different chord; each distinct chord has its own bit. */
static const u32 ctrl_chords[] = {
    PSP_CTRL_CROSS | PSP_CTRL_TRIANGLE,
    PSP_CTRL_LTRIGGER | PSP_CTRL_UP,
    PSP_CTRL_LTRIGGER | PSP_CTRL_DOWN,
    PSP_CTRL_LTRIGGER | PSP_CTRL_SQUARE,
    PSP_CTRL_LTRIGGER | PSP_CTRL_SELECT,
    PSP_CTRL_LTRIGGER | PSP_CTRL_TRIANGLE,
    PSP_CTRL_LTRIGGER | PSP_CTRL_START,
    PSP_CTRL_CROSS | PSP_CTRL_UP,
    PSP_CTRL_CROSS | PSP_CTRL_DOWN,
    PSP_CTRL_CROSS | PSP_CTRL_RTRIGGER,
    PSP_CTRL_CROSS | PSP_CTRL_LTRIGGER,
    PSP_CTRL_CROSS | PSP_CTRL_SQUARE,
    PSP_CTRL_CROSS | PSP_CTRL_CIRCLE,
    PSP_CTRL_CROSS | PSP_CTRL_SELECT,
    PSP_CTRL_CROSS | PSP_CTRL_START
};

struct ctrl_event { u32 buttons, timestamp; };
struct ctrl_state {
    int initialized;
    volatile int running;
    SceUID thread, lock, event_flag;
    struct ctrl_event queue[CTRL_QUEUE_CAPACITY];
    unsigned int front, rear, size;
    u32 overflow_buttons, overflow_chords, overflow_timestamp;
    SceCtrlData latest;
    u32 last_buttons, consumed_buttons;
    u32 repeat_button, repeat_deadline;
    enum ctrl_context context;
#ifdef ENABLE_HPRM
    int hprm_enabled;
    u32 last_hprm_key, hprm_buttons;
#endif
};
static struct ctrl_state g_ctrl;
static volatile unsigned int g_ctrl_last_activity_us;
static int ctrl_lock(void) { return sceKernelWaitSema(g_ctrl.lock, 1, 0) >= 0; }
static void ctrl_unlock(void) { sceKernelSignalSema(g_ctrl.lock, 1); }

static void ctrl_enqueue_locked(u32 buttons, u32 timestamp)
{
    unsigned int i;
    if (g_ctrl.size == CTRL_QUEUE_CAPACITY) {
        g_ctrl.overflow_timestamp = timestamp;
        if (!(buttons & (buttons - 1U))) g_ctrl.overflow_buttons |= buttons;
        else for (i = 0; i < sizeof(ctrl_chords) / sizeof(ctrl_chords[0]); ++i)
            if (buttons == ctrl_chords[i]) { g_ctrl.overflow_chords |= 1U << i; break; }
        return;
    }
    g_ctrl.queue[g_ctrl.rear].buttons = buttons;
    g_ctrl.queue[g_ctrl.rear].timestamp = timestamp;
    g_ctrl.rear = (g_ctrl.rear + 1U) % CTRL_QUEUE_CAPACITY;
    ++g_ctrl.size;
}
static int ctrl_dequeue_locked(struct ctrl_event *event)
{
    if (g_ctrl.size) {
        if (event) *event = g_ctrl.queue[g_ctrl.front];
        g_ctrl.front = (g_ctrl.front + 1U) % CTRL_QUEUE_CAPACITY;
        --g_ctrl.size;
        return 1;
    }
    if (g_ctrl.overflow_chords) {
        unsigned int i;
        for (i = 0; i < sizeof(ctrl_chords) / sizeof(ctrl_chords[0]); ++i)
            if (g_ctrl.overflow_chords & (1U << i)) {
                if (event) {
                    event->buttons = ctrl_chords[i];
                    event->timestamp = g_ctrl.overflow_timestamp;
                }
                g_ctrl.overflow_chords &= ~(1U << i);
                return 1;
            }
    }
    if (g_ctrl.overflow_buttons) {
        u32 single = g_ctrl.overflow_buttons & (0U - g_ctrl.overflow_buttons);
        if (event) { event->buttons = single; event->timestamp = g_ctrl.overflow_timestamp; }
        g_ctrl.overflow_buttons &= ~single;
        return 1;
    }
    return 0;
}

static u32 ctrl_map_hprm(u32 key)
{
    switch (key) {
    case PSP_HPRM_FORWARD:
        return CTRL_FORWARD;
    case PSP_HPRM_BACK:
        return CTRL_BACK;
    case PSP_HPRM_PLAYPAUSE:
        return CTRL_PLAYPAUSE;
    default:
        return 0;
    }
}

static int ctrl_thread(SceSize args, void *argp)
{
    SceCtrlData sample;
    u32 previous_activity_buttons = 0;

    (void)args;
    (void)argp;
    memset(&sample, 0, sizeof(sample));

    while (g_ctrl.running) {
        int count = sceCtrlReadBufferPositive(&sample, 1);
        int queued = 0;
        u32 event_buttons;

        if (count <= 0) {
            /* The normal path blocks in sceCtrlReadBufferPositive().  Avoid
             * a hot error loop during suspend or a transient driver failure. */
            sceKernelDelayThread(1000);
            continue;
        }

        event_buttons = sample.Buttons;
#ifdef ENABLE_ANALOG
        if (sample.Lx < 65 || sample.Lx > 191 ||
            sample.Ly < 65 || sample.Ly > 191)
            event_buttons |= CTRL_ANALOG;
#endif

#ifdef ENABLE_HPRM
        if (g_ctrl.hprm_enabled && sceHprmIsRemoteExist()) {
            u32 key = 0;
            if (sceHprmPeekCurrentKey(&key) >= 0) {
                g_ctrl.last_hprm_key = key;
                g_ctrl.hprm_buttons = ctrl_map_hprm(key);
            }
            event_buttons |= g_ctrl.hprm_buttons;
        } else {
            g_ctrl.hprm_buttons = 0;
        }
#endif

        /* Publish before waiting for queue bookkeeping. Workers must see a
         * physical edge even while the UI has not yet consumed its event.
         * Releases also start a fresh dwell, including consumed chords. */
        if (event_buttons != 0 || previous_activity_buttons != event_buttons)
            __sync_lock_test_and_set(&g_ctrl_last_activity_us,
                                     sceKernelGetSystemTimeLow());
        previous_activity_buttons = event_buttons;

        if (!ctrl_lock())
            continue;

        g_ctrl.latest = sample;

        {
            static const u32 browser_chords[] = {
                PSP_CTRL_CROSS | PSP_CTRL_TRIANGLE,
                PSP_CTRL_LTRIGGER | PSP_CTRL_UP,
                PSP_CTRL_LTRIGGER | PSP_CTRL_DOWN,
                PSP_CTRL_LTRIGGER | PSP_CTRL_SQUARE,
                /* Reserved/consumed: L+Select no longer opens a menu. */
                PSP_CTRL_LTRIGGER | PSP_CTRL_SELECT,
                PSP_CTRL_LTRIGGER | PSP_CTRL_TRIANGLE,
                PSP_CTRL_LTRIGGER | PSP_CTRL_START
            };
            static const u32 playback_chords[] = {
                PSP_CTRL_CROSS | PSP_CTRL_UP,
                PSP_CTRL_CROSS | PSP_CTRL_DOWN,
                PSP_CTRL_CROSS | PSP_CTRL_RTRIGGER,
                PSP_CTRL_CROSS | PSP_CTRL_LTRIGGER,
                PSP_CTRL_CROSS | PSP_CTRL_SQUARE,
                PSP_CTRL_CROSS | PSP_CTRL_CIRCLE,
                PSP_CTRL_CROSS | PSP_CTRL_SELECT,
                PSP_CTRL_CROSS | PSP_CTRL_START
            };
            const u32 *chords = 0;
            unsigned int count = 0, i;
            u32 pressed = event_buttons & ~g_ctrl.last_buttons;
            u32 released = g_ctrl.last_buttons & ~event_buttons;
            u32 singles;
            if (g_ctrl.context == CTRL_CONTEXT_BROWSER) {
                chords = browser_chords;
                count = sizeof(browser_chords) / sizeof(browser_chords[0]);
            } else if (g_ctrl.context == CTRL_CONTEXT_PLAYBACK) {
                chords = playback_chords;
                count = sizeof(playback_chords) / sizeof(playback_chords[0]);
            }
            for (i = 0; i < count; ++i) {
                u32 chord = chords[i];
                if ((event_buttons & chord) == chord && (pressed & chord)) {
                    ctrl_enqueue_locked(chord, sample.TimeStamp);
                    g_ctrl.consumed_buttons |= chord;
                    queued = 1;
                }
            }
            /* Left/right have no playback chords. Emit exactly one edge on
             * down, then one fixed-size seek tick every 250 ms while held.
             * Schedule from this sample: a delayed sampler never catches up
             * with a burst of synthetic jumps. Other contexts retain their
             * release/chord contract, and a context flush cannot restart a
             * direction that was already held on entry. */
            if (g_ctrl.context == CTRL_CONTEXT_PLAYBACK) {
                u32 directions = event_buttons & (PSP_CTRL_LEFT | PSP_CTRL_RIGHT);
                u32 edge = pressed & directions & ~g_ctrl.consumed_buttons;
                if (directions == PSP_CTRL_LEFT || directions == PSP_CTRL_RIGHT) {
                    if (edge ||
                        (g_ctrl.repeat_button == directions &&
                         (int)(sample.TimeStamp - g_ctrl.repeat_deadline) >= 0)) {
                        ctrl_enqueue_locked(directions, sample.TimeStamp);
                        g_ctrl.repeat_button = directions;
                        g_ctrl.repeat_deadline = sample.TimeStamp + CTRL_SEEK_REPEAT_US;
                        queued = 1;
                    }
                } else {
                    g_ctrl.repeat_button = 0;
                }
                /* Direction releases never create a second jump. Opposite
                 * directions held simultaneously suspend repeating. */
                g_ctrl.consumed_buttons |= directions;
                if (released & g_ctrl.repeat_button)
                    g_ctrl.repeat_button = 0;
            }
            if (g_ctrl.context == CTRL_CONTEXT_BROWSER) {
                u32 directions = event_buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN);
                u32 edge = pressed & directions & ~g_ctrl.consumed_buttons;
                u32 modifiers = event_buttons &
                    ~(PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_HOLD | CTRL_ANALOG);
                if (!modifiers && (directions == PSP_CTRL_UP || directions == PSP_CTRL_DOWN)) {
                    if (edge ||
                        (g_ctrl.repeat_button == directions &&
                         (int)(sample.TimeStamp - g_ctrl.repeat_deadline) >= 0)) {
                        ctrl_enqueue_locked(directions, sample.TimeStamp);
                        g_ctrl.repeat_button = directions;
                        g_ctrl.repeat_deadline = sample.TimeStamp +
                            (edge ? CTRL_BROWSER_REPEAT_DELAY_US : CTRL_BROWSER_REPEAT_US);
                        queued = 1;
                    }
                } else {
                    g_ctrl.repeat_button = 0;
                }
                /* Chords win while L is held. Pressing L after an initial
                 * single step still emits the existing L+direction chord. */
                g_ctrl.consumed_buttons |= directions;
                if (released & g_ctrl.repeat_button)
                    g_ctrl.repeat_button = 0;
            }
            singles = released & ~g_ctrl.consumed_buttons;
            /* Separate releases remain separate commands even if sampled in
             * one tick. No held face-button state enters this queue. */
            while (singles) {
                u32 single = singles & (0U - singles);
                ctrl_enqueue_locked(single, sample.TimeStamp);
                singles &= ~single;
                queued = 1;
            }
            g_ctrl.consumed_buttons &= event_buttons;
            g_ctrl.last_buttons = event_buttons;
        }

        ctrl_unlock();

        if (queued)
            sceKernelSetEventFlag(g_ctrl.event_flag, CTRL_EVENT_AVAILABLE);
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

int ctrl_init(void)
{
    if (g_ctrl.initialized)
        return 1;

    memset(&g_ctrl, 0, sizeof(g_ctrl));
    __sync_lock_test_and_set(&g_ctrl_last_activity_us,
                             sceKernelGetSystemTimeLow());
    g_ctrl.thread = -1;
    g_ctrl.lock = -1;
    g_ctrl.event_flag = -1;

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    g_ctrl.lock = sceKernelCreateSema("ppa_ctrl_lock", 0, 1, 1, 0);
    if (g_ctrl.lock < 0)
        return 0;

    g_ctrl.event_flag = sceKernelCreateEventFlag("ppa_ctrl_events", 0, 0, 0);
    if (g_ctrl.event_flag < 0) {
        sceKernelDeleteSema(g_ctrl.lock);
        g_ctrl.lock = -1;
        return 0;
    }

    g_ctrl.running = 1;
    g_ctrl.thread = ppa_thread_create(PPA_THREAD_INPUT,
                                      "ppa_ctrl_sampler",
                                      ctrl_thread,
                                      CTRL_THREAD_STACK_SIZE,
                                      0);
    if (g_ctrl.thread < 0 || sceKernelStartThread(g_ctrl.thread, 0, 0) < 0) {
        g_ctrl.running = 0;
        if (g_ctrl.thread >= 0)
            sceKernelDeleteThread(g_ctrl.thread);
        sceKernelDeleteEventFlag(g_ctrl.event_flag);
        sceKernelDeleteSema(g_ctrl.lock);
        g_ctrl.thread = -1;
        g_ctrl.event_flag = -1;
        g_ctrl.lock = -1;
        return 0;
    }

    g_ctrl.initialized = 1;
    return 1;
}

void ctrl_destroy(void)
{
    if (!g_ctrl.initialized)
        return;

    g_ctrl.running = 0;

    if (g_ctrl.thread >= 0) {
        sceKernelTerminateDeleteThread(g_ctrl.thread);
        g_ctrl.thread = -1;
    }

    if (g_ctrl.event_flag >= 0) {
        sceKernelDeleteEventFlag(g_ctrl.event_flag);
        g_ctrl.event_flag = -1;
    }

    if (g_ctrl.lock >= 0) {
        sceKernelDeleteSema(g_ctrl.lock);
        g_ctrl.lock = -1;
    }

    g_ctrl.initialized = 0;

}

void ctrl_analog(int *x, int *y)
{
    SceCtrlData sample;

    if (x == 0 || y == 0)
        return;

    memset(&sample, 0, sizeof(sample));
    sample.Lx = 128;
    sample.Ly = 128;

    if (g_ctrl.lock >= 0 && ctrl_lock()) {
        sample = g_ctrl.latest;
        ctrl_unlock();
    }

    *x = (int)sample.Lx - 128;
    *y = (int)sample.Ly - 128;
}

unsigned int ctrl_idle_time_us(void)
{
    unsigned int last = __sync_fetch_and_add(&g_ctrl_last_activity_us, 0U);
    return (unsigned int)(sceKernelGetSystemTimeLow() - last);
}

u32 ctrl_read_cont(void)
{
    u32 buttons = 0;

    if (g_ctrl.lock >= 0 && ctrl_lock()) {
        buttons = g_ctrl.last_buttons;
        ctrl_unlock();
    }

    return buttons;
}

u32 ctrl_read(void)
{
    struct ctrl_event event;

    memset(&event, 0, sizeof(event));
    if (g_ctrl.lock < 0 || !ctrl_lock())
        return 0;

    ctrl_dequeue_locked(&event);
    ctrl_unlock();
    return event.buttons;
}

int ctrl_peek_sample(SceCtrlData *sample)
{
    if (sample == 0)
        return 0;

    memset(sample, 0, sizeof(*sample));
    sample->Lx = 128;
    sample->Ly = 128;

    if (g_ctrl.lock < 0 || !ctrl_lock())
        return 0;

    *sample = g_ctrl.latest;
    sample->Buttons = g_ctrl.last_buttons;
    ctrl_unlock();
    return 1;
}

int ctrl_read_sample(SceCtrlData *sample)
{
    struct ctrl_event event;
    int had_event;

    if (sample == 0)
        return 0;

    memset(sample, 0, sizeof(*sample));
    memset(&event, 0, sizeof(event));
    sample->Lx = 128;
    sample->Ly = 128;

    if (g_ctrl.lock < 0 || !ctrl_lock())
        return 0;

    *sample = g_ctrl.latest;
    had_event = ctrl_dequeue_locked(&event);
    if (had_event) {
        sample->Buttons = event.buttons;
        sample->TimeStamp = event.timestamp;
    }
    else {
        sample->Buttons = 0;
    }
    ctrl_unlock();
    return 1;
}

int ctrl_read_sample_wait(SceCtrlData *sample, unsigned int timeout_us)
{
    SceUInt timeout = timeout_us;
    u32 matched = 0;

    if (sample == 0)
        return 0;

    /* Avoid entering the kernel when a queued transition is already ready.
     * Otherwise sleep until input arrives or one display-period-equivalent
     * timeout expires, allowing the main Allegrex core to idle. */
    if (!ctrl_pending() && g_ctrl.event_flag >= 0) {
        sceKernelWaitEventFlagCB(
            g_ctrl.event_flag,
            CTRL_EVENT_AVAILABLE,
            PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR,
            &matched,
            timeout_us == 0 ? 0 : &timeout);
        
    }

    return ctrl_read_sample(sample);
}

u32 ctrl_wait(unsigned int timeout_us)
{
    struct ctrl_event event;
    u32 buttons = 0;
    SceUInt timeout = timeout_us;
    u32 matched = 0;

    if (g_ctrl.lock < 0 || g_ctrl.event_flag < 0)
        return 0;

    memset(&event, 0, sizeof(event));
    if (ctrl_lock()) {
        int found = ctrl_dequeue_locked(&event);
        ctrl_unlock();
        if (found)
            return event.buttons;
    }

    if (sceKernelWaitEventFlagCB(g_ctrl.event_flag,
                               CTRL_EVENT_AVAILABLE,
                               PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR,
                               &matched,
                               timeout_us == 0 ? 0 : &timeout) < 0)
        return 0;

    if (ctrl_lock()) {
        if (ctrl_dequeue_locked(&event))
            buttons = event.buttons;
        ctrl_unlock();
    }

    return buttons;
}

void ctrl_flush(void)
{
    if (g_ctrl.lock < 0 || g_ctrl.event_flag < 0)
        return;

    if (ctrl_lock()) {
        g_ctrl.front = 0;
        g_ctrl.rear = 0;
        g_ctrl.size = 0;
        g_ctrl.overflow_buttons = g_ctrl.overflow_chords = 0;
        g_ctrl.overflow_timestamp = 0;
        g_ctrl.consumed_buttons |= g_ctrl.last_buttons;
        g_ctrl.repeat_button = 0;
        ctrl_unlock();
    }

    /* Keep the current held-button state so a launch/exit press is not
     * synthesized again. Their later releases are consumed too. */
    sceKernelClearEventFlag(g_ctrl.event_flag, 0);
}

int ctrl_pending(void)
{
    int pending = 0;

    if (g_ctrl.lock >= 0 && ctrl_lock()) {
        pending = g_ctrl.size != 0 || g_ctrl.overflow_buttons != 0 || g_ctrl.overflow_chords != 0;
        ctrl_unlock();
    }

    return pending;
}

u32 ctrl_hprm(void)
{
#ifdef ENABLE_HPRM
    u32 key = 0;
    if (g_ctrl.hprm_enabled && sceHprmIsRemoteExist())
        sceHprmPeekCurrentKey(&key);
    return key;
#else
    return 0;
#endif
}

void ctrl_enablehprm(int enable)
{
#ifdef ENABLE_HPRM
    g_ctrl.hprm_enabled = (sceKernelDevkitVersion() < 0x02000010) && enable;
#else
    (void)enable;
#endif
}

enum ctrl_context ctrl_set_context(enum ctrl_context context)
{
    enum ctrl_context previous = CTRL_CONTEXT_BROWSER;
    if (g_ctrl.lock >= 0 && ctrl_lock()) {
        previous = g_ctrl.context;
        g_ctrl.context = context;
        g_ctrl.front = g_ctrl.rear = g_ctrl.size = 0;
        g_ctrl.overflow_buttons = g_ctrl.overflow_chords = 0;
        g_ctrl.consumed_buttons |= g_ctrl.last_buttons;
        g_ctrl.repeat_button = 0;
        ctrl_unlock();
    }
    return previous;
}
