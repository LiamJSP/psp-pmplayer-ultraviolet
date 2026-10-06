#include "audio_vfpu.h"
#include "cpu_clock.h"

#include "../common/mem64.h"
#include "../common/ppa_scratchpad.h"
#include "../common/ppa_thread_policy.h"

#include <pspkernel.h>
#include <psppower.h>

#include <stdint.h>
#include <string.h>

#ifndef PPA_ENABLE_VFPU_ACCEL
#define PPA_ENABLE_VFPU_ACCEL 1
#endif
#ifndef PPA_ENABLE_SCRATCHPAD_ACCEL
#define PPA_ENABLE_SCRATCHPAD_ACCEL 1
#endif
#ifndef PPA_ENABLE_VFPU_AUDIO
#define PPA_ENABLE_VFPU_AUDIO 1
#endif
#ifndef PPA_AUDIO_ACCEL_DEFAULT_MODE
#define PPA_AUDIO_ACCEL_DEFAULT_MODE 0
#endif
#ifndef PPA_AUDIO_ACCEL_SCALAR_WARMUP_BLOCKS
#define PPA_AUDIO_ACCEL_SCALAR_WARMUP_BLOCKS 8
#endif
#ifndef PPA_AUDIO_ACCEL_VFPU_PROBE_BLOCKS
#define PPA_AUDIO_ACCEL_VFPU_PROBE_BLOCKS 8
#endif
#ifndef PPA_AUDIO_ACCEL_VFPU_MAX_COST_Q8
/* Require at least a 12.5% recurring-cost reduction. The comparison includes
 * request signalling, two scheduler transitions, VFPU context state, and the
 * scratchpad kernel, so a nominally faster kernel cannot raise total power. */
#define PPA_AUDIO_ACCEL_VFPU_MAX_COST_Q8 224U
#endif
#ifndef PPA_AUDIO_ACCEL_VERIFY_MAX_DELTA
#define PPA_AUDIO_ACCEL_VERIFY_MAX_DELTA 1U
#endif
#ifndef PPA_AUDIO_ACCEL_VERIFY_INTERVAL
#define PPA_AUDIO_ACCEL_VERIFY_INTERVAL 128U
#endif
#ifndef PPA_AUDIO_ACCEL_WORKER_TIMEOUT_US
/* This is a fault/suspend guard, not a normal scheduling budget. A healthy
 * resample batch should finish far below it even at the lowest clock. */
#define PPA_AUDIO_ACCEL_WORKER_TIMEOUT_US 250000U
#endif

#define PPA_AUDIO_ACCEL_MODE_AUTO   0
#define PPA_AUDIO_ACCEL_MODE_SCALAR 1
#define PPA_AUDIO_ACCEL_MODE_VFPU   2

#define PPA_AUDIO_WORKER_STACK_BYTES 0x3000
#define PPA_AUDIO_VERIFY_POINTS      17U

/* PSPLink/debug builds use -O0. Keep the four-lane kernel wrapper inline so a
 * debug measurement does not pay one C call/return for every four samples and
 * falsely reject the VFPU plan or raise the clock governor's observed duty. */
#if defined(__GNUC__)
#define PPA_AUDIO_HOT_INLINE static inline __attribute__((always_inline))
#else
#define PPA_AUDIO_HOT_INLINE static inline
#endif

struct ppa_audio_plan_entry {
    uint16_t index;
    uint16_t reserved;
    uint32_t fraction;
};

/* 112 bytes, naturally fitting the fixed 2 KiB audio scratchpad partition.
 * Every vector begins on a 16-byte boundary for lv.q/sv.q. */
struct ppa_audio_vfpu_batch {
    int32_t left_a[4];       /* 0 */
    int32_t left_b[4];       /* 16 */
    int32_t right_a[4];      /* 32 */
    int32_t right_b[4];      /* 48 */
    float fraction[4];       /* 64 */
    int32_t left_delta[4];   /* 80 */
    int32_t right_delta[4];  /* 96 */
} __attribute__((aligned(16)));

typedef char ppa_audio_batch_size_check[
    sizeof(struct ppa_audio_vfpu_batch) == 112 ? 1 : -1];
typedef char ppa_audio_batch_fit_check[
    sizeof(struct ppa_audio_vfpu_batch) <= PPA_SCRATCHPAD_AUDIO_BYTES ? 1 : -1];

enum ppa_audio_plan_state {
    PPA_AUDIO_PLAN_DISABLED = 0,
    PPA_AUDIO_PLAN_SCALAR_WARMUP,
    PPA_AUDIO_PLAN_VFPU_PROBE,
    PPA_AUDIO_PLAN_SCALAR_LOCKED,
    PPA_AUDIO_PLAN_VFPU_LOCKED
};

struct ppa_audio_worker_request {
    short *dest;
    const short *src;
    const struct ppa_audio_plan_entry *plan;
    unsigned int input_frames;
    unsigned int output_frames;
    volatile int result;
};

struct ppa_audio_accel_state {
    struct ppa_audio_plan_entry *plan;
    unsigned int input_frames;
    unsigned int output_frames;
    enum ppa_audio_plan_state plan_state;
    int configured_mode;
    int active;

    SceUID worker_thread;
    SceUID worker_request_sema;
    SceUID worker_done_sema;
    volatile int worker_stop;
    struct ppa_audio_worker_request request;

    uint64_t scalar_cost_total_q8;
    uint64_t vfpu_cost_total_q8;
    unsigned int scalar_samples;
    unsigned int vfpu_samples;
    unsigned int scalar_blocks;
    unsigned int vfpu_blocks;
    unsigned int worker_creates;
    unsigned int worker_failures;
    unsigned int scratchpad_failures;
    unsigned int verification_passes;
    unsigned int verification_failures;
    unsigned int max_delta;
    unsigned int plan_changes;
};

static struct ppa_audio_accel_state g_audio_accel = {
    .worker_thread = -1,
    .worker_request_sema = -1,
    .worker_done_sema = -1
};

static uint64_t audio_now_us(void)
{
    return (uint64_t)sceKernelGetSystemTimeWide();
}

static void audio_set_plan(enum ppa_audio_plan_state state, const char *reason)
{
    if (g_audio_accel.plan_state == state)
        return;
    g_audio_accel.plan_state = state;
    g_audio_accel.plan_changes++;
    (void)reason;
}

static int build_plan(struct ppa_audio_plan_entry *plan,
                      unsigned int input_frames,
                      unsigned int output_frames)
{
    uint64_t step;
    uint64_t phase = 0ULL;
    unsigned int out;

    if (plan == 0 || input_frames < 2U || output_frames < 2U ||
        input_frames > 65535U)
        return 0;

    step = (((uint64_t)(input_frames - 1U)) << 32) /
           (uint64_t)(output_frames - 1U);
    for (out = 0U; out < output_frames; ++out) {
        if (out + 1U == output_frames) {
            plan[out].index = (uint16_t)(input_frames - 1U);
            plan[out].fraction = 0U;
        }
        else {
            unsigned int index = (unsigned int)(phase >> 32);
            if (index >= input_frames)
                index = input_frames - 1U;
            plan[out].index = (uint16_t)index;
            plan[out].fraction = (uint32_t)phase;
            phase += step;
        }
        plan[out].reserved = 0U;
    }
    return 1;
}

static void scalar_plan_resample(short *dest,
                                 const short *src,
                                 const struct ppa_audio_plan_entry *plan,
                                 unsigned int input_frames,
                                 unsigned int output_frames)
{
    unsigned int out;
    for (out = 0U; out < output_frames; ++out) {
        unsigned int index = plan[out].index;
        unsigned int next = index + 1U < input_frames ? index + 1U : index;
        uint32_t fraction = plan[out].fraction;
        int channel;
        for (channel = 0; channel < 2; ++channel) {
            int32_t a = src[index * 2U + (unsigned int)channel];
            int32_t b = src[next * 2U + (unsigned int)channel];
            int64_t delta = (int64_t)(b - a) * (int64_t)(uint64_t)fraction;
            dest[out * 2U + (unsigned int)channel] =
                (short)(a + (int32_t)(delta >> 32));
        }
    }
}

static int16_t scalar_plan_sample(const short *src,
                                  const struct ppa_audio_plan_entry *entry,
                                  unsigned int input_frames,
                                  unsigned int channel)
{
    unsigned int index = entry->index;
    unsigned int next = index + 1U < input_frames ? index + 1U : index;
    int32_t a = src[index * 2U + channel];
    int32_t b = src[next * 2U + channel];
    int64_t delta = (int64_t)(b - a) *
                    (int64_t)(uint64_t)entry->fraction;
    return (int16_t)(a + (int32_t)(delta >> 32));
}

static unsigned int verify_output(const short *dest,
                                  const short *src,
                                  const struct ppa_audio_plan_entry *plan,
                                  unsigned int input_frames,
                                  unsigned int output_frames)
{
    unsigned int point;
    unsigned int max_delta = 0U;
    if (output_frames == 0U)
        return 0U;
    for (point = 0U; point < PPA_AUDIO_VERIFY_POINTS; ++point) {
        unsigned int out = PPA_AUDIO_VERIFY_POINTS == 1U ? 0U :
            (unsigned int)(((uint64_t)(output_frames - 1U) * point) /
                           (PPA_AUDIO_VERIFY_POINTS - 1U));
        unsigned int channel;
        for (channel = 0U; channel < 2U; ++channel) {
            int expected = scalar_plan_sample(src, &plan[out],
                                              input_frames, channel);
            int actual = dest[out * 2U + channel];
            unsigned int delta = (unsigned int)(actual >= expected ?
                actual - expected : expected - actual);
            if (delta > max_delta)
                max_delta = delta;
        }
    }
    return max_delta;
}

static uint64_t normalized_cost_q8(unsigned int elapsed_us,
                                   unsigned int output_frames)
{
    int cpu = scePowerGetCpuClockFrequencyInt();
    if (cpu <= 0)
        cpu = 333;
    if (output_frames == 0U)
        output_frames = 1U;
    return ((uint64_t)elapsed_us * (unsigned int)cpu * 256ULL +
            output_frames / 2U) / output_frames;
}

PPA_AUDIO_HOT_INLINE void vfpu_interpolate_batch(
    struct ppa_audio_vfpu_batch *batch)
{
#if PPA_ENABLE_VFPU_ACCEL && PPA_ENABLE_SCRATCHPAD_ACCEL && PPA_ENABLE_VFPU_AUDIO
    /* The scalar gather retains exact 32.32 source-position semantics. VFPU
     * performs four left and four right interpolation deltas together. vf2id
     * deliberately rounds toward -infinity, matching signed arithmetic >>32
     * for negative products in the authoritative scalar implementation. */
    __asm__ volatile(
        "lv.q C000, 0(%0)\n"
        "lv.q C010, 16(%0)\n"
        "lv.q C020, 32(%0)\n"
        "lv.q C030, 48(%0)\n"
        "vi2f.q C000, C000, 0\n"
        "vi2f.q C010, C010, 0\n"
        "vi2f.q C020, C020, 0\n"
        "vi2f.q C030, C030, 0\n"
        "lv.q C100, 64(%0)\n"
        "vsub.q C110, C010, C000\n"
        "vsub.q C120, C030, C020\n"
        "vmul.q C110, C110, C100\n"
        "vmul.q C120, C120, C100\n"
        "vf2id.q C110, C110, 0\n"
        "vf2id.q C120, C120, 0\n"
        "sv.q C110, 80(%0)\n"
        "sv.q C120, 96(%0)\n"
        /* The CPU consumes these scratchpad words immediately after return.
         * Drain the VFPU write buffer so no stale lane can be observed. */
        "vflush\n"
        :
        : "r"(batch)
        : "memory");
#else
    (void)batch;
#endif
}

static int vfpu_resample_kernel(short *dest,
                                const short *src,
                                const struct ppa_audio_plan_entry *plan,
                                unsigned int input_frames,
                                unsigned int output_frames)
{
#if PPA_ENABLE_VFPU_ACCEL && PPA_ENABLE_SCRATCHPAD_ACCEL && PPA_ENABLE_VFPU_AUDIO
    struct ppa_scratchpad_lease lease;
    struct ppa_audio_vfpu_batch *batch;
    unsigned int out;

    memset(&lease, 0, sizeof(lease));
    if (!ppa_scratchpad_acquire(PPA_SCRATCHPAD_PARTITION_AUDIO, &lease)) {
        g_audio_accel.scratchpad_failures++;
        return 0;
    }
    if (lease.bytes < sizeof(*batch)) {
        ppa_scratchpad_release(&lease);
        g_audio_accel.scratchpad_failures++;
        return 0;
    }
    batch = (struct ppa_audio_vfpu_batch *)lease.base;

    for (out = 0U; out < output_frames; out += 4U) {
        unsigned int lane;
        unsigned int count = output_frames - out;
        if (count > 4U)
            count = 4U;
        for (lane = 0U; lane < 4U; ++lane) {
            if (lane < count) {
                const struct ppa_audio_plan_entry *entry = &plan[out + lane];
                unsigned int index = entry->index;
                unsigned int next = index + 1U < input_frames ?
                    index + 1U : index;
                batch->left_a[lane] = src[index * 2U];
                batch->left_b[lane] = src[next * 2U];
                batch->right_a[lane] = src[index * 2U + 1U];
                batch->right_b[lane] = src[next * 2U + 1U];
                batch->fraction[lane] =
                    (float)entry->fraction * 2.3283064365386962890625e-10f;
            }
            else {
                batch->left_a[lane] = 0;
                batch->left_b[lane] = 0;
                batch->right_a[lane] = 0;
                batch->right_b[lane] = 0;
                batch->fraction[lane] = 0.0f;
            }
        }
        vfpu_interpolate_batch(batch);
        for (lane = 0U; lane < count; ++lane) {
            int32_t left = batch->left_a[lane] + batch->left_delta[lane];
            int32_t right = batch->right_a[lane] + batch->right_delta[lane];
            if (left < -32768) left = -32768;
            if (left > 32767) left = 32767;
            if (right < -32768) right = -32768;
            if (right > 32767) right = 32767;
            dest[(out + lane) * 2U] = (short)left;
            dest[(out + lane) * 2U + 1U] = (short)right;
        }
    }

    ppa_scratchpad_release(&lease);
    return 1;
#else
    (void)dest;
    (void)src;
    (void)plan;
    (void)input_frames;
    (void)output_frames;
    return 0;
#endif
}

static int audio_worker_thread(SceSize input_length, void *input)
{
    (void)input_length;
    (void)input;
    for (;;) {
        if (sceKernelWaitSema(g_audio_accel.worker_request_sema,
                              1, 0) < 0)
            break;
        if (g_audio_accel.worker_stop)
            break;
        g_audio_accel.request.result = vfpu_resample_kernel(
            g_audio_accel.request.dest,
            g_audio_accel.request.src,
            g_audio_accel.request.plan,
            g_audio_accel.request.input_frames,
            g_audio_accel.request.output_frames);
        (void)sceKernelSignalSema(g_audio_accel.worker_done_sema, 1);
    }
    return 0;
}

static void stop_worker(void)
{
    if (g_audio_accel.worker_thread >= 0) {
        g_audio_accel.worker_stop = 1;
        if (g_audio_accel.worker_request_sema >= 0)
            (void)sceKernelSignalSema(g_audio_accel.worker_request_sema, 1);
        (void)sceKernelWaitThreadEnd(g_audio_accel.worker_thread, 0);
        (void)sceKernelDeleteThread(g_audio_accel.worker_thread);
    }
    if (g_audio_accel.worker_request_sema >= 0)
        (void)sceKernelDeleteSema(g_audio_accel.worker_request_sema);
    if (g_audio_accel.worker_done_sema >= 0)
        (void)sceKernelDeleteSema(g_audio_accel.worker_done_sema);
    g_audio_accel.worker_thread = -1;
    g_audio_accel.worker_request_sema = -1;
    g_audio_accel.worker_done_sema = -1;
    g_audio_accel.worker_stop = 0;
    memset(&g_audio_accel.request, 0, sizeof(g_audio_accel.request));
}

static int ensure_worker(void)
{
#if PPA_ENABLE_VFPU_ACCEL && PPA_ENABLE_SCRATCHPAD_ACCEL && PPA_ENABLE_VFPU_AUDIO
    if (g_audio_accel.worker_thread >= 0)
        return 1;
    g_audio_accel.worker_request_sema =
        sceKernelCreateSema("ppa_audio_vfpu_req", 0, 0, 1, 0);
    g_audio_accel.worker_done_sema =
        sceKernelCreateSema("ppa_audio_vfpu_done", 0, 0, 1, 0);
    if (g_audio_accel.worker_request_sema < 0 ||
        g_audio_accel.worker_done_sema < 0)
        goto fail;
    g_audio_accel.worker_thread = ppa_thread_create(
        PPA_THREAD_AUDIO_RESAMPLE,
        "ppa_audio_vfpu",
        audio_worker_thread,
        PPA_AUDIO_WORKER_STACK_BYTES,
        PSP_THREAD_ATTR_VFPU);
    if (g_audio_accel.worker_thread < 0)
        goto fail;
    if (sceKernelStartThread(g_audio_accel.worker_thread, 0, 0) < 0) {
        (void)sceKernelDeleteThread(g_audio_accel.worker_thread);
        g_audio_accel.worker_thread = -1;
        goto fail;
    }
    g_audio_accel.worker_creates++;
    return 1;

fail:
    g_audio_accel.worker_failures++;
    stop_worker();
    return 0;
#else
    return 0;
#endif
}

static int submit_worker(short *dest,
                         const short *src,
                         const struct ppa_audio_plan_entry *plan,
                         unsigned int input_frames,
                         unsigned int output_frames)
{
    SceUInt timeout;
    if (!ensure_worker())
        return 0;
    g_audio_accel.request.dest = dest;
    g_audio_accel.request.src = src;
    g_audio_accel.request.plan = plan;
    g_audio_accel.request.input_frames = input_frames;
    g_audio_accel.request.output_frames = output_frames;
    g_audio_accel.request.result = 0;
    if (sceKernelSignalSema(g_audio_accel.worker_request_sema, 1) < 0)
        return 0;
    timeout = (SceUInt)PPA_AUDIO_ACCEL_WORKER_TIMEOUT_US;
    if (sceKernelWaitSema(g_audio_accel.worker_done_sema, 1, &timeout) < 0) {
        /* Once the request semaphore was signalled, the worker may still be
         * writing the destination. Join it before the caller runs scalar
         * fallback, preventing concurrent writes after a timeout or wakeup
         * interruption. */
        stop_worker();
        return 0;
    }
    return g_audio_accel.request.result != 0;
}

static void decide_auto_plan(void)
{
    uint64_t scalar_average;
    uint64_t vfpu_average;
    if (g_audio_accel.scalar_samples == 0U ||
        g_audio_accel.vfpu_samples == 0U)
        return;
    scalar_average = g_audio_accel.scalar_cost_total_q8 /
                     g_audio_accel.scalar_samples;
    vfpu_average = g_audio_accel.vfpu_cost_total_q8 /
                   g_audio_accel.vfpu_samples;
    if (vfpu_average * 256ULL <=
        scalar_average * (uint64_t)PPA_AUDIO_ACCEL_VFPU_MAX_COST_Q8) {
        audio_set_plan(PPA_AUDIO_PLAN_VFPU_LOCKED,
                       "measured_power_margin");
    }
    else {
        audio_set_plan(PPA_AUDIO_PLAN_SCALAR_LOCKED,
                       "insufficient_total_savings");
        stop_worker();
    }
}

void ppa_audio_accel_session_begin(unsigned int input_frames,
                                   unsigned int output_frames,
                                   int resample_required)
{
    size_t bytes;
    ppa_audio_accel_session_end();
    memset(&g_audio_accel, 0, sizeof(g_audio_accel));
    g_audio_accel.worker_thread = -1;
    g_audio_accel.worker_request_sema = -1;
    g_audio_accel.worker_done_sema = -1;
    g_audio_accel.configured_mode = PPA_AUDIO_ACCEL_DEFAULT_MODE;
    if (g_audio_accel.configured_mode < PPA_AUDIO_ACCEL_MODE_AUTO ||
        g_audio_accel.configured_mode > PPA_AUDIO_ACCEL_MODE_VFPU)
        g_audio_accel.configured_mode = PPA_AUDIO_ACCEL_MODE_AUTO;

    if (!resample_required || input_frames < 2U || output_frames < 2U ||
        input_frames == output_frames || input_frames > 65535U)
        return;

    bytes = (size_t)output_frames * sizeof(struct ppa_audio_plan_entry);
    if (bytes == 0U || bytes > 0x7fffffffU)
        return;
    g_audio_accel.plan = (struct ppa_audio_plan_entry *)malloc_64((int)bytes);
    if (g_audio_accel.plan == 0)
        return;
    if (!build_plan(g_audio_accel.plan, input_frames, output_frames)) {
        free_64(g_audio_accel.plan);
        g_audio_accel.plan = 0;
        return;
    }
    g_audio_accel.input_frames = input_frames;
    g_audio_accel.output_frames = output_frames;
    g_audio_accel.active = 1;

#if PPA_ENABLE_VFPU_ACCEL && PPA_ENABLE_SCRATCHPAD_ACCEL && PPA_ENABLE_VFPU_AUDIO
    ppa_scratchpad_init();
    ppa_scratchpad_set_phase(PPA_SCRATCHPAD_PHASE_PLAYBACK);
    if (g_audio_accel.configured_mode == PPA_AUDIO_ACCEL_MODE_SCALAR) {
        audio_set_plan(PPA_AUDIO_PLAN_SCALAR_LOCKED, "forced_scalar");
    }
    else if (!ensure_worker()) {
        audio_set_plan(PPA_AUDIO_PLAN_SCALAR_LOCKED, "worker_unavailable");
    }
    else if (g_audio_accel.configured_mode == PPA_AUDIO_ACCEL_MODE_VFPU) {
        audio_set_plan(PPA_AUDIO_PLAN_VFPU_PROBE, "forced_vfpu_preflight");
    }
    else {
        /* The worker is created before playback threads start and then sleeps
         * through scalar warmup. This avoids a mid-stream thread-creation stall
         * while still measuring every recurring VFPU handoff/context cost. */
        audio_set_plan(PPA_AUDIO_PLAN_SCALAR_WARMUP, "scalar_default");
    }
#else
    audio_set_plan(PPA_AUDIO_PLAN_SCALAR_LOCKED, "vfpu_build_disabled");
#endif

    ;
}

void ppa_audio_accel_session_end(void)
{
    if (g_audio_accel.worker_thread >= 0 ||
        g_audio_accel.worker_request_sema >= 0 ||
        g_audio_accel.worker_done_sema >= 0)
        stop_worker();
    if (g_audio_accel.plan != 0)
        free_64(g_audio_accel.plan);
    memset(&g_audio_accel, 0, sizeof(g_audio_accel));
    g_audio_accel.worker_thread = -1;
    g_audio_accel.worker_request_sema = -1;
    g_audio_accel.worker_done_sema = -1;
}

int ppa_audio_accel_resample(short *dest,
                             const short *src,
                             unsigned int input_frames,
                             unsigned int output_frames)
{
    uint64_t started;
    unsigned int elapsed;

    if (!g_audio_accel.active || g_audio_accel.plan == 0 ||
        dest == 0 || src == 0 ||
        input_frames != g_audio_accel.input_frames ||
        output_frames != g_audio_accel.output_frames)
        return 0;

    if (g_audio_accel.plan_state == PPA_AUDIO_PLAN_VFPU_PROBE ||
        g_audio_accel.plan_state == PPA_AUDIO_PLAN_VFPU_LOCKED) {
        unsigned int delta = 0U;
        int verify_now = g_audio_accel.plan_state == PPA_AUDIO_PLAN_VFPU_PROBE ||
            g_audio_accel.vfpu_blocks == 0U ||
            (PPA_AUDIO_ACCEL_VERIFY_INTERVAL != 0U &&
             (g_audio_accel.vfpu_blocks % PPA_AUDIO_ACCEL_VERIFY_INTERVAL) == 0U);
        int ok;
        int worker_ok;
        int verify_failed = 0;
        started = audio_now_us();
        ok = submit_worker(dest, src, g_audio_accel.plan,
                           input_frames, output_frames);
        worker_ok = ok;
        elapsed = (unsigned int)(audio_now_us() - started);
        cpu_clock_auto_on_audio_resample_us(elapsed);

        if (ok && verify_now) {
            delta = verify_output(dest, src, g_audio_accel.plan,
                                  input_frames, output_frames);
            if (delta > g_audio_accel.max_delta)
                g_audio_accel.max_delta = delta;
            if (delta > PPA_AUDIO_ACCEL_VERIFY_MAX_DELTA) {
                verify_failed = 1;
                ok = 0;
            }
            else {
                g_audio_accel.verification_passes++;
            }
        }
        if (!ok) {
            if (!worker_ok) {
                g_audio_accel.worker_failures++;
            }
            if (verify_failed) {
                g_audio_accel.verification_failures++;
            }
            started = audio_now_us();
            scalar_plan_resample(dest, src, g_audio_accel.plan,
                                 input_frames, output_frames);
            elapsed = (unsigned int)(audio_now_us() - started);
            cpu_clock_auto_on_audio_resample_us(elapsed);
            g_audio_accel.scalar_blocks++;
            audio_set_plan(PPA_AUDIO_PLAN_SCALAR_LOCKED,
                           "vfpu_verify_or_worker_failure");
            stop_worker();
            return 1;
        }

        g_audio_accel.vfpu_blocks++;
        if (g_audio_accel.plan_state == PPA_AUDIO_PLAN_VFPU_PROBE) {
            g_audio_accel.vfpu_cost_total_q8 +=
                normalized_cost_q8(elapsed, output_frames);
            g_audio_accel.vfpu_samples++;
            if (g_audio_accel.configured_mode == PPA_AUDIO_ACCEL_MODE_VFPU &&
                g_audio_accel.vfpu_samples >= 2U) {
                audio_set_plan(PPA_AUDIO_PLAN_VFPU_LOCKED,
                               "forced_vfpu_verified");
            }
            else if (g_audio_accel.configured_mode == PPA_AUDIO_ACCEL_MODE_AUTO &&
                     g_audio_accel.vfpu_samples >=
                         PPA_AUDIO_ACCEL_VFPU_PROBE_BLOCKS) {
                decide_auto_plan();
            }
        }
        return 1;
    }

    started = audio_now_us();
    scalar_plan_resample(dest, src, g_audio_accel.plan,
                         input_frames, output_frames);
    elapsed = (unsigned int)(audio_now_us() - started);
    cpu_clock_auto_on_audio_resample_us(elapsed);
    g_audio_accel.scalar_blocks++;

    if (g_audio_accel.plan_state == PPA_AUDIO_PLAN_SCALAR_WARMUP) {
        g_audio_accel.scalar_cost_total_q8 +=
            normalized_cost_q8(elapsed, output_frames);
        g_audio_accel.scalar_samples++;
        if (g_audio_accel.scalar_samples >=
            PPA_AUDIO_ACCEL_SCALAR_WARMUP_BLOCKS) {
            if (g_audio_accel.worker_thread >= 0)
                audio_set_plan(PPA_AUDIO_PLAN_VFPU_PROBE,
                               "scalar_baseline_ready");
            else
                audio_set_plan(PPA_AUDIO_PLAN_SCALAR_LOCKED,
                               "worker_unavailable");
        }
    }
    return 1;
}
