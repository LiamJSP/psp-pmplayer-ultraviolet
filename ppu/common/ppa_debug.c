/* Optional main-CPU diagnostics. Sony owns ME/VME throughout. No logger,
 * exception path or callback resets a codec or releases hardware-owned data. */
#include "ppa_debug.h"
#include "ppa_debug_state.h"
#include "ppa_playback_session.h"
#include "ppa_thread_policy.h"
#include "ppa_hardware_profile.h"
#include "ppa_privileged_bridge.h"
#include "ppa_wait.h"
#include "../mod/h264x_probe.h"
#include "cooleyesBridge.h"
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <psprtc.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define DEBUG_DIR "ms0:/PSP/PPU_DEBUG"
#define DEBUG_CRASH DEBUG_DIR "/crash.log"
#define RECORDS 32U
#define RECORD_BYTES 384U
#define CRASH_LIMIT (256U * 1024U)
#define CSV_LIMIT (2U * 1024U * 1024U)
struct debug_record { unsigned int csv; char text[RECORD_BYTES]; };
static struct debug_record ring[RECORDS];
static unsigned int head, tail, dropped;
static volatile unsigned int enabled, accepting, stop_logger, fault_ready;
static volatile unsigned int logger_park; /* 0 running, 1 requested, 2 parked */
static SceUID logger = -1, crash_fd = -1, csv_fd = -1;
static unsigned int crash_bytes, csv_bytes;
static struct ppa_debug_state state;
static int state_slot = -1, movie, started, exception_installed;
static uint64_t last_sample, last_emit;
static unsigned int last_presented, last_skipped, last_paused;
static unsigned int b_count, ref_b_count, max_b_run, b_run, parsed_aus, parse_failures;
static unsigned int encoder_probes, encoder_fields;
static PspDebugRegBlock fault_regs;
static SceUID fault_thread;

/* PSPSDK's exception assembly/trap lives in this user module. Only registration
 * goes through the kernel bridge, avoiding a kernel-only import in the EBOOT.
 * Do not move the trap into the PRX: eret must return to user-accessible code. */
int sceKernelRegisterDefaultExceptionHandler(void *handler)
{
    if (!(ppa_privileged_bridge_capabilities() & COOLEYES_BRIDGE_CAP_DEBUG_EXCEPTION)) return -1;
    return cooleyesDebugExceptionHandler(handler, 1);
}
extern void _pspDebugExceptionHandler(void);

static void debug_exception(PspDebugRegBlock *regs)
{
    /* PSPSDK has returned from raw exception context. No heap, stdio, GU,
     * mutex or storage operation here; the pre-existing logger owns writes.
     * A ruined stack, kernel/ME fault or broken scheduler can defeat this. */
    if (__sync_bool_compare_and_swap(&fault_ready, 0U, 1U)) {
        fault_regs = *regs;
        fault_thread = sceKernelGetThreadId();
        __sync_synchronize();
        __sync_lock_test_and_set(&fault_ready, 2U);
    }
    /* Never return into PSPSDK's ExitDeleteThread: retain firmware ownership,
     * the faulted owner's stack and all buffers until a cold restart. */
    for (;;) sceKernelSleepThread();
}

static int write_all(SceUID fd, const void *data, unsigned int bytes)
{
    const unsigned char *p = data;
    while (bytes) {
        int n = sceIoWrite(fd, p, bytes);
        if (n <= 0 || (unsigned int)n > bytes) return 0;
        p += n; bytes -= (unsigned int)n;
    }
    return 1;
}
static void record_line(unsigned int csv, const char *line)
{
    int irq;
    unsigned int next, i;
    if (!accepting || !line) return;
    irq = sceKernelCpuSuspendIntr();
    next = (head + 1U) % RECORDS;
    if (next == tail) ++dropped;
    else {
        ring[head].csv = csv;
        for (i = 0; i + 2U < RECORD_BYTES && line[i]; ++i) {
            unsigned char c = (unsigned char)line[i];
            ring[head].text[i] = c < 32U ? ' ' : (char)c;
        }
        ring[head].text[i++] = '\n';
        ring[head].text[i] = 0;
        head = next;
    }
    sceKernelCpuResumeIntr(irq);
}
void ppa_debug_event(const char *message) { record_line(0U, message); }
void ppa_debug_error(const char *where, int code)
{
    char line[RECORD_BYTES];
    if (!accepting) return;
    snprintf(line, sizeof(line), "error time_us=%llu thread=%d code=0x%08x %s",
        (unsigned long long)sceKernelGetSystemTimeWide(), sceKernelGetThreadId(),
        (unsigned)code, where ? where : "unknown");
    record_line(0U, line);
}
static void output(SceUID *fd, unsigned int *bytes, unsigned int cap, const char *line)
{
    unsigned int n = (unsigned int)strlen(line);
    if (*fd < 0) return;
    if (*bytes >= cap || n > cap - *bytes) return; /* bounded for even an indefinitely paused movie */
    if (!write_all(*fd, line, n)) {
        sceIoClose(*fd); *fd = -1; /* no retry storm on full/removed cards */
    } else *bytes += n;
}
static void drain(void)
{
    struct debug_record r;
    char line[RECORD_BYTES];
    unsigned int budget = RECORDS;
    if (__sync_fetch_and_add(&fault_ready, 0U) == 2U) {
        unsigned int i;
        snprintf(line, sizeof(line),
            "exception thread=%d EPC=%08x cause=%08x status=%08x badvaddr=%08x; restart required\n",
            fault_thread, (unsigned)fault_regs.epc, (unsigned)fault_regs.cause,
            (unsigned)fault_regs.status, (unsigned)fault_regs.badvaddr);
        output(&crash_fd, &crash_bytes, CRASH_LIMIT, line);
        for (i = 0; i < 32U; i += 4U) {
            snprintf(line, sizeof(line), "r%u=%08x r%u=%08x r%u=%08x r%u=%08x\n",
                i, (unsigned)fault_regs.r[i], i+1U, (unsigned)fault_regs.r[i+1],
                i+2U, (unsigned)fault_regs.r[i+2], i+3U, (unsigned)fault_regs.r[i+3]);
            output(&crash_fd, &crash_bytes, CRASH_LIMIT, line);
        }
        (void)sceIoSync("ms0:", 0);
        __sync_lock_test_and_set(&fault_ready, 3U);
    }
    while (budget--) {
        int irq = sceKernelCpuSuspendIntr();
        if (tail == head) { sceKernelCpuResumeIntr(irq); break; }
        r = ring[tail]; tail = (tail + 1U) % RECORDS;
        sceKernelCpuResumeIntr(irq);
        if (r.csv) output(&csv_fd, &csv_bytes, CSV_LIMIT, r.text);
        else output(&crash_fd, &crash_bytes, CRASH_LIMIT - 2048U, r.text);
    }
}
static int logger_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!stop_logger) {
        unsigned int state = __sync_val_compare_and_swap(&logger_park, 1U, 2U);
        if (state == 1U || state == 2U) {
            sceKernelDelayThread(5000U);
            continue;
        }
        drain(); sceKernelDelayThread(100000U);
    }
    drain();
    return 0;
}
int ppa_debug_park(void)
{
    if (logger < 0) return 1;
    (void)__sync_val_compare_and_swap(&logger_park, 0U, 1U);
    return __sync_fetch_and_add(&logger_park, 0U) == 2U;
}
void ppa_debug_unpark(void) { __sync_lock_test_and_set(&logger_park, 0U); }
static int logger_start(void)
{
    ppa_debug_unpark();
    stop_logger = 0;
    logger = ppa_thread_create(PPA_THREAD_BACKGROUND, "PPU diagnostics", logger_main, 8192, 0);
    if (logger < 0) return 0;
    if (sceKernelStartThread(logger, 0, 0) < 0) {
        sceKernelDeleteThread(logger); logger = -1; return 0;
    }
    accepting = 1U;
    return 1;
}
static void logger_stop(void)
{
    ppa_debug_unpark();
    accepting = 0U;
    if (logger >= 0) {
        stop_logger = 1U;
        if (ppa_wait_thread_end_safe(logger, "debug", "writer", 1000000U, 250000U) < 0)
            ppa_wait_quarantine("Diagnostic writer could not stop", -1);
        sceKernelDeleteThread(logger); logger = -1;
    }
}
static void close_logs(void)
{
    if (csv_fd >= 0) { sceIoClose(csv_fd); csv_fd = -1; }
    if (crash_fd >= 0) { sceIoClose(crash_fd); crash_fd = -1; }
}
static int persist_state(void)
{
    char path[80];
    struct ppa_debug_state next = state;
    int slot = state_slot == 0 ? 1 : 0, ok;
    SceUID fd;
    ++next.generation;
    ppa_debug_state_seal(&next);
    snprintf(path, sizeof(path), DEBUG_DIR "/state%d.dat", slot);
    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (fd < 0) return 0;
    ok = write_all(fd, &next, sizeof(next));
    if (sceIoClose(fd) < 0) ok = 0;
    if (sceIoSync("ms0:", 0) < 0) ok = 0;
    if (!ok) return 0;
    state = next; state_slot = slot;
    return 1;
}
static void exception_disable(void)
{
    if (exception_installed) {
        int result = cooleyesDebugExceptionHandler(
            (void *)((uintptr_t)_pspDebugExceptionHandler | 0x80000000U), 0);
        /* Do not unload registered code even if firmware refuses removal. */
        if (result < 0) ppa_wait_quarantine("Exception handler could not detach", result);
        exception_installed = 0;
    }
}
void ppa_debug_shutdown(void)
{
    exception_disable();
    enabled = 0U;
    logger_stop(); close_logs();
}
static int capture_start(void)
{
    SceOff size;
    sceIoMkdir("ms0:/PSP", 0777);
    sceIoMkdir(DEBUG_DIR, 0777);
    crash_fd = sceIoOpen(DEBUG_CRASH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0666);
    if (crash_fd < 0) return 0;
    size = sceIoLseek(crash_fd, 0, PSP_SEEK_END);
    /* Leave space for a final exception even after a long prior capture. */
    if (size < 0 || size >= CRASH_LIMIT / 2U) {
        sceIoClose(crash_fd);
        sceIoRemove(DEBUG_DIR "/crash.previous.log");
        (void)sceIoRename(DEBUG_CRASH, DEBUG_DIR "/crash.previous.log");
        crash_fd = sceIoOpen(DEBUG_CRASH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
        size = 0;
    }
    crash_bytes = (unsigned int)size;
    if (crash_fd < 0 || !logger_start()) { close_logs(); return 0; }
    enabled = 1U;
    if (pspDebugInstallErrorHandler(debug_exception) >= 0) exception_installed = 1;
    else ppa_debug_event("exception handler unavailable; ordinary errors and interrupted-session marker remain active");
    return 1;
}
int ppa_debug_enabled(void) { return enabled != 0U; }
unsigned int ppa_debug_remaining(void) { return state.remaining; }
void ppa_debug_init(int configured)
{
    int i;
    memset(&state, 0, sizeof(state)); state_slot = -1;
    for (i = 0; i < 2; ++i) {
        char path[80]; struct ppa_debug_state candidate;
        SceUID fd; int n;
        snprintf(path, sizeof(path), DEBUG_DIR "/state%d.dat", i);
        fd = sceIoOpen(path, PSP_O_RDONLY, 0);
        if (fd < 0) continue;
        n = sceIoRead(fd, &candidate, sizeof(candidate)); sceIoClose(fd);
        if (n == sizeof(candidate) && ppa_debug_state_valid(&candidate) &&
            (state_slot < 0 || ppa_debug_state_newer(&candidate, &state))) {
            state = candidate; state_slot = i;
        }
    }
    /* A stale TRUE in config must never refill an exhausted/corrupt state.
     * Explicit menu confirmation is the only way to replenish the allowance. */
    if (!configured || state_slot < 0 || !state.enabled) return;
    if (state.pending) {
        /* Reserve-at-start accounts for the interrupted attempt. */
        if (capture_start()) {
            ppa_debug_event("previous playback interrupted (crash, power loss, or forced exit); reserved play counted");
            logger_stop(); close_logs(); exception_disable(); enabled = 0U;
        }
        state.pending = 0U;
        if (!state.remaining) state.enabled = 0U;
        if (!persist_state()) return;
    }
    if (state.remaining && state.enabled) (void)capture_start();
}
int ppa_debug_set_enabled(int value)
{
    if (movie) return 0; /* menu changes only between sessions */
    ppa_debug_shutdown();
    sceIoMkdir("ms0:/PSP", 0777); sceIoMkdir(DEBUG_DIR, 0777);
    state.enabled = value != 0; state.remaining = value ? 3U : 0U; state.pending = 0U;
    if (!persist_state()) return 0;
    if (value && !capture_start()) {
        state.enabled = state.remaining = 0U; (void)persist_state(); return 0;
    }
    return 1;
}
void ppa_debug_movie_begin(const char *path, const char *container, int audio_only)
{
    char line[RECORD_BYTES], filename[96];
    const struct ppa_hardware_profile *hw;
    ScePspDateTime date;
    if (!enabled) return;
    logger_stop();
    head = tail = dropped = 0U;
    b_count = ref_b_count = max_b_run = b_run = parsed_aus = parse_failures = 0U;
    encoder_probes = encoder_fields = 0U;
    last_sample = last_emit = 0; last_presented = last_skipped = last_paused = 0U;
    csv_bytes = 0; started = 0; movie = 1;
    snprintf(filename, sizeof(filename), DEBUG_DIR "/playback-%u.csv", (3U - state.remaining) % 3U + 1U);
    csv_fd = sceIoOpen(filename, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
    if (csv_fd >= 0) {
        const char *header = "time_us,video_ms,audio_ms,audio_valid,paused,audio_only,cpu_mhz,bus_mhz,presented_delta,skipped_delta,decode_samples,decode_mean_us,decode_peak_us,budget_us,audio_queued,partition_free,partition_max,source_refs,effective_refs,compatibility,weights_disabled,parsed_aus,b_pictures,reference_b_pictures,max_consecutive_b_observed,parse_failures,log_records_dropped\n";
        if (write_all(csv_fd, header, strlen(header))) csv_bytes = strlen(header);
        else { sceIoClose(csv_fd); csv_fd = -1; }
    }
    if (!logger_start()) { movie = 0; ppa_debug_shutdown(); return; }
    ppa_hardware_profile_refresh_memory(); hw = ppa_hardware_profile_get();
    memset(&date, 0, sizeof(date)); sceRtcGetCurrentClockLocalTime(&date);
    snprintf(line, sizeof(line), "begin %04u-%02u-%02uT%02u:%02u:%02u local container=%s audio_only=%d file=%.240s",
        (unsigned)date.year, (unsigned)date.month, (unsigned)date.day,
        (unsigned)date.hour, (unsigned)date.minute, (unsigned)date.second,
        container, audio_only, path ? path : "unknown");
    ppa_debug_event(line);
    snprintf(line, sizeof(line), "hardware model=%s generation=%d expected_ram_mb=%u ge_edram_bytes=%u extended_ram=%u partition_free=%u partition_max=%u firmware=%08x",
        ppa_hardware_model_name(), hw->model_generation, hw->expected_physical_ram_mb,
        hw->ge_edram_bytes, hw->extended_user_memory_visible, hw->current_total_free_bytes,
        hw->current_max_free_bytes, (unsigned)sceKernelDevkitVersion());
    ppa_debug_event(line);
    ppa_debug_event("H264 encoder settings unknown unless SEI reports them; reference B pictures indicate pyramid use; observed B run is a lower bound, not encoder bframes");
    if (csv_fd < 0) ppa_debug_error("cannot create playback CSV", -1);
}
void ppa_debug_movie_started(void)
{
    if (!enabled || !movie || started) return;
    if (!state.remaining) { ppa_debug_shutdown(); return; }
    --state.remaining; state.pending = 1U;
    if (!persist_state()) { ppa_debug_shutdown(); return; }
    started = 1;
}
void ppa_debug_movie_start_failed(void)
{
    if (started && state.remaining < 3U) {
        ++state.remaining; state.pending = 0U;
        if (!persist_state()) ppa_debug_shutdown();
    }
    started = 0;
}
void ppa_debug_movie_end(const char *result)
{
    char line[RECORD_BYTES];
    if (!movie) return;
    if (result && strncmp(result, "exit:", 5)) ppa_debug_error(result, -1);
    snprintf(line, sizeof(line), "end result=%s remaining=%u parsed_aus=%u b_pictures=%u reference_b_pictures=%u max_consecutive_b_observed=%u b_pyramid_observed=%s dropped=%u",
        result ? result : "finished", state.remaining, parsed_aus, b_count, ref_b_count, max_b_run,
        ref_b_count ? "yes" : "not_observed", dropped);
    ppa_debug_event(line);
    logger_stop();
    if (csv_fd >= 0) { sceIoClose(csv_fd); csv_fd = -1; }
    if (started) {
        state.pending = 0U;
        if (!state.remaining) state.enabled = 0U;
        if (!persist_state()) state.enabled = 0U;
    }
    movie = started = 0;
    if (!state.enabled || !enabled) ppa_debug_shutdown();
    else if (!logger_start()) ppa_debug_shutdown();
}
void ppa_debug_media_info(unsigned int width, unsigned int height,
    unsigned int rate, unsigned int scale, unsigned int duration_ms,
    unsigned int audio_codec, unsigned int audio_rate, unsigned int audio_channels,
    int battery_saver)
{
    char line[RECORD_BYTES];
    if (!accepting || !movie) return;
    snprintf(line, sizeof(line), "media width=%u height=%u fps_num=%u fps_den=%u duration_ms=%u audio_fourcc=%08x source_audio_hz=%u channels=%u battery_saver=%d",
        width, height, rate, scale, duration_ms, audio_codec, audio_rate, audio_channels, battery_saver);
    ppa_debug_event(line);
    if (battery_saver) ppa_debug_event("battery saver bypasses AU inspection; B-frame/pyramid observations and encoder SEI may be unavailable");
}

void ppa_debug_video(const struct h264x_probe *p)
{
    char line[RECORD_BYTES];
    if (!accepting || !movie || !p) return;
    if (!p->sps.valid || !p->pps.valid) { ppa_debug_event("H264 SPS/PPS metadata unavailable (parse failed)"); return; }
    snprintf(line, sizeof(line), "H264 source profile_idc=%u level_idc=%u constraints=%02x coded=%ux%u display=%ux%u chroma_format_idc=%u bit_depth=%u/%u progressive=%u refs=%u cabac=%u weighted_pred=%u weighted_bipred=%u transform8x8=%u nal_length_size=%u",
        p->sps.profile_idc, p->sps.level_idc, p->sps.constraint_flags,
        p->sps.coded_width, p->sps.coded_height, p->sps.display_width, p->sps.display_height,
        p->sps.chroma_format_idc, 8U+p->sps.bit_depth_luma_minus8, 8U+p->sps.bit_depth_chroma_minus8,
        p->sps.frame_mbs_only_flag, p->sps.num_ref_frames, p->pps.entropy_coding_mode_flag,
        p->pps.weighted_pred_flag, p->pps.weighted_bipred_idc, p->pps.transform_8x8_mode_flag, p->nal_length_size);
    ppa_debug_event(line);
}
void ppa_debug_note_au(const struct h264x_au_info *au)
{
    int irq;
    if (!enabled || !movie || !started) return;
    irq = sceKernelCpuSuspendIntr();
    if (!au->valid || au->parse_error) { ++parse_failures; b_run = 0U; }
    else {
        ++parsed_aus;
        if (au->is_b) { ++b_count; if (++b_run > max_b_run) max_b_run = b_run; }
        else b_run = 0U;
        if (au->is_reference_b) ++ref_b_count;
    }
    sceKernelCpuResumeIntr(irq);
}
void ppa_debug_discontinuity(void) { if (enabled) b_run = 0U; }
void ppa_debug_tick(int force)
{
    struct ppa_session_snapshot snap;
    uint64_t now;
    char line[RECORD_BYTES];
    unsigned int bc, rb, mb, au, pe, lost;
    int irq;
    if (!enabled || !movie) return;
    now = sceKernelGetSystemTimeWide();
    if (!force && last_sample && now - last_sample < 1000000ULL) return;
    last_sample = now;
    if (!ppa_session_debug_snapshot(&snap)) return;
    if (!force && snap.paused && last_paused && snap.presented == last_presented &&
        snap.skipped == last_skipped && now - last_emit < 10000000ULL) return;
    irq = sceKernelCpuSuspendIntr();
    bc=b_count; rb=ref_b_count; mb=max_b_run; au=parsed_aus; pe=parse_failures; lost=dropped;
    sceKernelCpuResumeIntr(irq);
    snprintf(line, sizeof(line), "%llu,%lld,%lld,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u,%u,%u,%u,%u,%u,%u",
        (unsigned long long)now, (long long)snap.video_ms, (long long)snap.audio_ms,
        snap.audio_valid, snap.paused, snap.audio_only, scePowerGetCpuClockFrequencyInt(),
        scePowerGetBusClockFrequencyInt(), snap.presented-last_presented, snap.skipped-last_skipped,
        snap.decode_samples, snap.decode_samples ? (unsigned)(snap.decode_total_us/snap.decode_samples) : 0U,
        snap.decode_peak_us, snap.budget_us, ppa_session_audio_only_queued_blocks(),
        (unsigned)sceKernelTotalFreeMemSize(), (unsigned)sceKernelMaxFreeMemSize(),
        snap.original_refs, snap.effective_refs, snap.compatibility, snap.weights_disabled,
        au, bc, rb, mb, pe, lost);
    record_line(1U, line);
    last_presented=snap.presented; last_skipped=snap.skipped; last_paused=snap.paused; last_emit=now;
}

/* Only inspect the first 32 submitted AUs, and at most 4 KiB of any SEI NAL.
 * This is advisory encoder metadata, never an input to compatibility policy. */
void ppa_debug_encoder_options(const void *data, unsigned int bytes, unsigned int length_size)
{
    const unsigned char *p = data;
    unsigned int pos = 0;
    if (!enabled || !movie || encoder_probes++ >= 32U || !p || length_size < 1U || length_size > 4U) return;
    while (pos < bytes && length_size <= bytes-pos) {
        unsigned int n = 0, i;
        for (i=0; i<length_size; ++i) n = (n << 8) | p[pos++];
        if (!n || n > bytes-pos) return;
        if ((p[pos] & 31U) == 6U) {
            char rbsp[4096]; unsigned int r=0, z=0, k=1, off=0;
            for (; k<n && r+1U<sizeof(rbsp); ++k) {
                unsigned char c=p[pos+k];
                if (z>=2U && c==3U) { z=0; continue; }
                rbsp[r++]=(char)c; z=c==0 ? z+1U : 0U;
            }
            rbsp[r]=0;
            while (off<r) {
                unsigned int type=0, size=0, b;
                do { if (off>=r) return; b=(unsigned char)rbsp[off++]; type+=b; } while (b==255U);
                do { if (off>=r) return; b=(unsigned char)rbsp[off++]; size+=b; } while (b==255U);
                if (size>r-off) break;
                if (type==5U && size>16U) {
                    char line[RECORD_BYTES]; unsigned int j, end=off+size;
                    /* user_data_unregistered starts with a 16-byte UUID. */
                    for (j=off+16U; j+8U<end; ++j) {
                        unsigned int field = !memcmp(rbsp+j, "bframes=", 8) ? 1U :
                            (j+10U<=end && !memcmp(rbsp+j, "b_pyramid=", 10)) ? 2U : 0U;
                        if (field && !(encoder_fields & field)) {
                            unsigned int len=0;
                            encoder_fields |= field;
                            while (j+len<end && len<48U && (unsigned char)rbsp[j+len]>32U) ++len;
                            snprintf(line, sizeof(line), "H264 encoder_SEI %.*s (encoder-reported, not independently verified)", (int)len, rbsp+j);
                            ppa_debug_event(line);
                        }
                    }
                }
                off+=size;
            }
        }
        pos+=n;
    }
}
