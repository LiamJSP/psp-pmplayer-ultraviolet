#include <assert.h>
#include <stdio.h>
#include "../ppu/common/ppa_playback_session.c"
#include "../ppu/common/ppa_playback_control.c"
const char *__asan_default_options(void) { return "detect_leaks=0"; }
static int lowered, boosted, rest, wake_mode;
static int native_request, requests, unlocks, locks, unlock_error, lock_error;
static int io_idle = 1, debug_idle = 1, io_parked, debug_parked, cache_flushes;
static int instant_resume, display_restores, rapid_repause;
static void *display_frame = (void *)0x04000000;
static volatile int *observed_pause;
static uint64_t test_clock=1000000;
SceInt64 sceKernelGetSystemTimeWide(void) { return test_clock; }
SceUID sceKernelCreateSema(const char *s,SceUInt a,int b,int c,SceKernelSemaOptParam *p)
{ (void)s;(void)a;(void)b;(void)c;(void)p;return 7; }
int sceKernelPollSema(SceUID s,int n) { (void)s;(void)n;return 0; }
int sceKernelWaitSema(SceUID s,int n,SceUInt *t) { (void)s;(void)n;(void)t;return 0; }
int sceKernelSignalSema(SceUID s,int n) { (void)s;(void)n;return 0; }
int sceKernelDeleteSema(SceUID s) { (void)s;return 0; }
int sceKernelDelayThread(SceUInt n)
{
    test_clock+=n;
    if (rapid_repause == 1) { g_idle_workers = 0; rapid_repause = 2; return 0; }
    if (rapid_repause == 2) {
        assert(g_idle_workers & PPA_SESSION_WORKER_DECODE);
        rapid_repause = 0;
    }
    if (wake_mode) *observed_pause=0;
    return 0;
}
int sceAudioGetChannelRestLength(int c) { (void)c;return rest; }
int scePowerLock(int n) { (void)n; ++locks; return lock_error ? -1 : 0; }
int scePowerUnlock(int n)
{
    (void)n; ++unlocks;
    assert(g_suspend_requested && g_lock_borrowed);
    assert(g_options.browser_power_lock_owned && !*g_options.browser_power_lock_owned);
    assert(*observed_pause && rest == 0 && io_parked && debug_parked && cache_flushes);
    if (unlock_error) return -1;
    if (instant_resume) {
        /* Unlock can return only AFTER sleep and wake; the old ordering sent
         * another suspend request after this point. */
        test_clock += 3600000000ULL;
        native_request = 0;
        ppa_session_power_callback(PSP_POWER_CB_SUSPENDING | PSP_POWER_CB_RESUME_COMPLETE);
    }
    return 0;
}
int scePowerTick(int n) { (void)n;return 0; }
int scePowerIsRequest(void) { return native_request; }
int scePowerRequestSuspend(void)
{
    assert(g_suspend_requested);
    assert(!g_options.browser_power_lock_owned || *g_options.browser_power_lock_owned);
    ++requests; native_request = 1; return 0;
}
int ppa_io_pump_park(struct PpaIoPump *p) { (void)p; io_parked = 1; return io_idle; }
int ppa_io_pump_unpark(struct PpaIoPump *p) { (void)p; io_parked = 0; return 1; }
int ppa_debug_park(void) { debug_parked = 1; return debug_idle; }
void ppa_debug_unpark(void) { debug_parked = 0; }
void ppa_cache_wbinv_all(void) { ++cache_flushes; }
int ppa_privileged_display_set_enabled(int n) { (void)n;return 0; }
int sceDisplayGetFrameBuf(void **p,int *pitch,int *format,int sync)
{ (void)sync;*p=display_frame;*pitch=512;*format=3;return 0; }
int sceDisplaySetFrameBuf(void *p,int pitch,int format,int sync)
{ (void)sync; if (p) { assert(p == display_frame && pitch == 512 && format == 3); ++display_restores; } return 0; }
void cpu_clock_auto_pause(void) { ++lowered; }
int cpu_clock_get_auto_enabled(void) { return 1; }
void cpu_clock_auto_boost_for_reason(cpu_clock_auto_boost_reason r)
{ (void)r;assert(*observed_pause==1);++boosted; }
int ppa_debug_enabled(void) { return 0; }
void ppa_debug_event(const char *s) { (void)s; }
void ppa_debug_error(const char *s,int n) { (void)s; (void)n; }
void ppa_debug_tick(int force) { (void)force; }

static void apply_pause(SceCtrlData *input, volatile int *paused)
{
    int action = ppa_session_service(input, *paused);
    if (action & PPA_SESSION_PAUSE) {
        *paused = 1; ppa_playback_control_pause();
    }
}
static void finish_wake(SceCtrlData *input, volatile int *paused, int *owned)
{
    int old_requests = requests;
    input->Buttons = PSP_CTRL_SQUARE; /* waking with Play held must not run */
    apply_pause(input, paused);
    assert(*paused && *owned && !g_lock_borrowed && !g_suspend_pending);
    assert(requests == old_requests && !ppa_session_accept_input(PSP_CTRL_SQUARE));
    assert(display_restores && io_parked && debug_parked);
    test_clock += 400000;
    input->Buttons = 0; apply_pause(input, paused);
    assert(!ppa_session_accept_input(PSP_CTRL_RIGHT));
    assert(!ppa_session_accept_input(PSP_CTRL_LTRIGGER));
    assert(ppa_session_accept_input(PSP_CTRL_SQUARE));
    assert(!io_parked && !debug_parked);
    ppa_playback_control_toggle_pause(paused);
    assert(!*paused);
}
static void test_standby(SceCtrlData *input, volatile int *paused)
{
    int owned = 1, before;
    struct ppa_session_options options = {0,0,0,&owned};
    unsigned int all = PPA_SESSION_WORKER_AUDIO | PPA_SESSION_WORKER_VIDEO | PPA_SESSION_WORKER_DECODE;
    wake_mode = 0; *paused = 0;
    ppa_session_configure(&options); assert(ppa_session_open() == 0); ppa_session_start();
    /* The switch can be queued under the browser's lock before its callback. */
    native_request = 1;
    apply_pause(input, paused); assert(*paused && owned && !unlocks && g_suspend_pending);
    g_idle_workers = all & ~PPA_SESSION_WORKER_DECODE;
    apply_pause(input, paused); assert(!unlocks);
    g_idle_workers = all; rest = 128;
    apply_pause(input, paused); assert(!unlocks);
    rest = -1; apply_pause(input, paused); assert(!unlocks); /* failed DMA query is not idle */
    rest = 0; io_idle = 0; apply_pause(input, paused); assert(!unlocks);
    io_idle = 1; debug_idle = 0; apply_pause(input, paused); assert(!unlocks);
    debug_idle = 1; instant_resume = 1;
    apply_pause(input, paused); assert(unlocks == 1 && !owned && !requests);
    finish_wake(input, paused, &owned); assert(locks == 1 && !requests);
    ppa_session_close(); assert(owned && locks == 1);

    /* A timer requests exactly once, before unlock. Hours asleep must neither
     * duplicate the request nor immediately refire the timer after waking. */
    options.sleep_minutes = 15; ppa_session_configure(&options);
    assert(ppa_session_open() == 0); ppa_session_start();
    test_clock += 900000000ULL; apply_pause(input, paused); assert(*paused);
    g_idle_workers = all; before = requests; apply_pause(input, paused);
    assert(requests == before + 1 && !owned);
    finish_wake(input, paused, &owned);
    apply_pause(input, paused); assert(!*paused && requests == before + 1);
    ppa_session_close();

    /* Already-paused audio-only playback has no video worker to acknowledge. */
    options.sleep_minutes = 0; options.audio_only = 1;
    ppa_session_configure(&options); assert(ppa_session_open() == 0); ppa_session_start();
    *paused = 1; g_idle_workers = PPA_SESSION_WORKER_AUDIO | PPA_SESSION_WORKER_DECODE;
    ppa_session_power_callback(PSP_POWER_CB_POWER_SWITCH); native_request = 1;
    before = requests; unlock_error = 1;
    apply_pause(input, paused); assert(owned && !g_lock_borrowed && requests == before);
    unlock_error = 0; apply_pause(input, paused); assert(!owned);
    finish_wake(input, paused, &owned); assert(requests == before);
    ppa_session_close();

    /* Coalesced switch/wake flags and a stale native request cannot send a
     * fully resumed unit straight back to sleep. */
    ppa_session_configure(&options); assert(ppa_session_open() == 0);
    native_request = 1; before = unlocks;
    ppa_session_power_callback(PSP_POWER_CB_POWER_SWITCH | PSP_POWER_CB_RESUME_COMPLETE);
    apply_pause(input, paused); apply_pause(input, paused);
    assert(*paused && unlocks == before && !g_suspend_pending);
    native_request = 0; ppa_session_close();
    /* A forced native suspend can arrive without a switch request. No input
     * may release paused workers until RESUME_COMPLETE has been observed. */
    ppa_session_configure(&options); assert(ppa_session_open() == 0);
    ppa_session_power_callback(PSP_POWER_CB_SUSPENDING);
    apply_pause(input, paused);
    test_clock += 400000;
    apply_pause(input, paused);
    assert(!ppa_session_accept_input(PSP_CTRL_SQUARE));
    assert(!ppa_session_accept_input(PSP_CTRL_TRIANGLE));
    ppa_session_power_callback(PSP_POWER_CB_RESUME_COMPLETE);
    apply_pause(input, paused);
    test_clock += 400000; input->Buttons = 0; apply_pause(input, paused);
    assert(ppa_session_accept_input(PSP_CTRL_TRIANGLE));
    ppa_session_close();
    puts("PASS: switch/poll standby, worker+DMA+I/O drain, immediate wake, one-shot timer, retained frame, fresh Play gate and balanced power locks");
}
int main(void)
{
    struct ppa_session_options options={0,0,0,0};
    SceCtrlData input={0};volatile int paused=0,stop=0;
    observed_pause=&paused;input.Lx=input.Ly=128;
    ppa_session_configure(&options);assert(ppa_session_open()==0);
    ppa_playback_control_toggle_pause(&paused);assert(paused && lowered==0);
    g_idle_workers=PPA_SESSION_WORKER_AUDIO | PPA_SESSION_WORKER_VIDEO;
    ppa_session_service(&input,1);assert(lowered==0); /* decoder still in firmware */
    g_idle_workers|=PPA_SESSION_WORKER_DECODE;rest=128;
    ppa_session_service(&input,1);assert(lowered==0); /* audio DMA tail */
    rest=0;ppa_session_service(&input,1);assert(lowered==1);
    ppa_session_service(&input,1);assert(lowered==1); /* only once per pause */
    ppa_playback_control_toggle_pause(&paused);assert(!paused && boosted==1 && !g_idle_workers);
    ppa_playback_control_toggle_pause(&paused);ppa_session_service(&input,1);
    assert(lowered==1); /* old pause acknowledgement cannot authorize a new drop */
    wake_mode=1;
    rapid_repause=1;
    assert(ppa_session_pause_point(&stop,&paused,PPA_SESSION_WORKER_DECODE)==2);
    assert(!g_idle_workers); /* caller can reset its watchdog after parked time */
    assert(ppa_session_pause_point(&stop,&paused,PPA_SESSION_WORKER_DECODE)==1);
    stop=1;assert(ppa_session_pause_point(&stop,&paused,PPA_SESSION_WORKER_DECODE)==0);
    ppa_session_close();
    options.audio_only=1;ppa_session_configure(&options);assert(ppa_session_open()==0);
    paused=1;g_idle_workers=PPA_SESSION_WORKER_AUDIO|PPA_SESSION_WORKER_DECODE;
    ppa_session_service(&input,1);assert(lowered==2); /* no nonexistent video worker */
    ppa_playback_control_unpause(&paused);assert(!paused && boosted==2);
    ppa_session_close();
    puts("PASS: pause quiescence, DMA drain, resume ordering, rapid toggles, audio-only and parked-time reporting");
    test_standby(&input, &paused);
    return 0;
}
