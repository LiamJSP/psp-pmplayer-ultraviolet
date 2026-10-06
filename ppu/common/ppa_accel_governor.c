#include "ppa_accel_governor.h"
#include "ppa_hardware_profile.h"

#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef PPA_ENABLE_ACCEL_GOVERNOR
#define PPA_ENABLE_ACCEL_GOVERNOR 1
#endif
#ifndef PPA_ENABLE_VFPU_ACCEL
#define PPA_ENABLE_VFPU_ACCEL 1
#endif
#ifndef PPA_ENABLE_VFPU_VIDEO
#define PPA_ENABLE_VFPU_VIDEO 1
#endif
#ifndef PPA_ENABLE_SCRATCHPAD_ACCEL
#define PPA_ENABLE_SCRATCHPAD_ACCEL 1
#endif
#ifndef PPA_ACCEL_VERIFY_MAX_DELTA
#define PPA_ACCEL_VERIFY_MAX_DELTA 3
#endif
#ifndef PPA_ENABLE_CPU_FULL_FRAME_FILTERS
#define PPA_ENABLE_CPU_FULL_FRAME_FILTERS 1
#endif
#ifndef PPA_ACCEL_DEFAULT_MODE
#define PPA_ACCEL_DEFAULT_MODE PPA_ACCEL_MODE_AUTO
#endif
#ifndef PPA_ACCEL_PROFILE_BUILD_ID
#define PPA_ACCEL_PROFILE_BUILD_ID 0x00020001U
#endif

#define PPA_ACCEL_MIN_PLAN_HOLD_US 4000000ULL
#define PPA_ACCEL_REPROBE_US      10000000ULL
#define PPA_ACCEL_GOOD_MARGIN_Q8  236U /* VFPU cost <= 92.2% of scalar. */
#define PPA_ACCEL_BAD_MARGIN_Q8   269U /* VFPU cost >= 105.1% of scalar. */

#define PPA_ACCEL_PROFILE_PATH "ms0:/PSP/SYSTEM/ppa_accel_profile_v1.bin"
#define PPA_ACCEL_PROFILE_TEMP "ms0:/PSP/SYSTEM/ppa_accel_profile_v1.tmp"
#define PPA_ACCEL_PROFILE_MAGIC 0x43434150U /* "PACC" little-endian. */
#define PPA_ACCEL_PROFILE_VERSION 1U

struct ppa_accel_profile {
    uint32_t magic;
    uint32_t version;
    uint32_t bytes;
    uint32_t hardware_model;
    uint32_t build_id;
    uint32_t scalar_sample_ewma_us;
    uint32_t vfpu_sample_ewma_us;
    uint32_t stage_mask;
    uint32_t preferred_plan;
    uint32_t sample_count;
    uint32_t crc32;
};

struct ppa_accel_state {
    enum ppa_accel_mode configured_mode;
    enum ppa_accel_plan active_plan;
    uint64_t last_change_us;
    uint64_t next_probe_us;
    unsigned int vfpu_frame_ewma_us;
    unsigned int scalar_frame_ewma_us;
    unsigned int transfer_ewma_us;
    unsigned int scalar_sample_ewma_us;
    unsigned int vfpu_sample_ewma_us;
    unsigned int estimated_scalar_frame_us;
    unsigned int last_cost_ratio_q8;
    unsigned int decisions;
    unsigned int plan_changes;
    unsigned int vfpu_frames;
    unsigned int scalar_frames;
    unsigned int good_windows;
    unsigned int bad_windows;
    unsigned int calibration_samples;
    unsigned int verification_failures;
    unsigned int current_stage_mask;
    unsigned int profile_stage_mask;
    unsigned int pending_profile_scalar_us;
    unsigned int pending_profile_vfpu_us;
    enum ppa_accel_plan pending_profile_plan;
    int probe_frame;
    int initialized;
    int profile_loaded;
    int profile_pending;
    int profile_write_attempted;
    int profile_dirty;
};

static struct ppa_accel_state g_accel;

static uint64_t accel_now(void)
{
    return (uint64_t)sceKernelGetSystemTimeWide();
}

static unsigned int ewma8(unsigned int old_value, unsigned int sample)
{
    if (old_value == 0U)
        return sample;
    return (old_value * 7U + sample + 4U) >> 3;
}

static uint32_t profile_crc32(const void *data, unsigned int bytes)
{
    uint32_t crc = 0xffffffffU;
    const unsigned char *p = (const unsigned char *)data;
    unsigned int bit;
    while (bytes-- != 0U) {
        crc ^= *p++;
        for (bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

static int read_exact(SceUID fd, void *data, unsigned int bytes)
{
    unsigned char *p = (unsigned char *)data;
    while (bytes != 0U) {
        int got = sceIoRead(fd, p, bytes);
        if (got <= 0)
            return 0;
        p += got;
        bytes -= (unsigned int)got;
    }
    return 1;
}

static int write_exact(SceUID fd, const void *data, unsigned int bytes)
{
    const unsigned char *p = (const unsigned char *)data;
    while (bytes != 0U) {
        int put = sceIoWrite(fd, p, bytes);
        if (put <= 0)
            return 0;
        p += put;
        bytes -= (unsigned int)put;
    }
    return 1;
}

static uint32_t profile_build_key(void)
{
    uint32_t key = (uint32_t)PPA_ACCEL_PROFILE_BUILD_ID;
    key ^= (uint32_t)(PPA_ENABLE_VFPU_ACCEL & 1) << 1;
    key ^= (uint32_t)(PPA_ENABLE_VFPU_VIDEO & 1) << 2;
    key ^= (uint32_t)(PPA_ENABLE_SCRATCHPAD_ACCEL & 1) << 3;
    key ^= (uint32_t)(PPA_ENABLE_ACCEL_GOVERNOR & 1) << 4;
    key ^= (uint32_t)(PPA_ENABLE_CPU_FULL_FRAME_FILTERS & 1) << 5;
    key ^= (uint32_t)(PPA_ACCEL_DEFAULT_MODE & 3) << 6;
    key ^= (uint32_t)(PPA_ACCEL_VERIFY_MAX_DELTA & 0xff) << 8;
    return key ^ 0x9e3779b9U;
}

static int load_profile(void)
{
    struct ppa_accel_profile profile;
    SceUID fd = sceIoOpen(PPA_ACCEL_PROFILE_PATH, PSP_O_RDONLY, 0);
    uint32_t expected;
    if (fd < 0)
        return 0;
    memset(&profile, 0, sizeof(profile));
    if (!read_exact(fd, &profile, sizeof(profile))) {
        sceIoClose(fd);
        return 0;
    }
    sceIoClose(fd);
    expected = profile_crc32(&profile,
                             (unsigned int)offsetof(struct ppa_accel_profile,
                                                    crc32));
    if (profile.magic != PPA_ACCEL_PROFILE_MAGIC ||
        profile.version != PPA_ACCEL_PROFILE_VERSION ||
        profile.bytes != sizeof(profile) ||
        profile.hardware_model != (uint32_t)ppa_hardware_model() ||
        profile.build_id != profile_build_key() ||
        profile.crc32 != expected ||
        profile.scalar_sample_ewma_us == 0U ||
        profile.vfpu_sample_ewma_us == 0U ||
        profile.stage_mask == 0U ||
        profile.preferred_plan > PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR)
        return 0;

    g_accel.profile_stage_mask = profile.stage_mask;
    g_accel.pending_profile_scalar_us = profile.scalar_sample_ewma_us;
    g_accel.pending_profile_vfpu_us = profile.vfpu_sample_ewma_us;
    g_accel.pending_profile_plan =
        (enum ppa_accel_plan)profile.preferred_plan;
    g_accel.calibration_samples = profile.sample_count < 3U ?
        profile.sample_count : 3U;
    g_accel.profile_loaded = 1;
    g_accel.profile_pending = 1;
    return 1;
}

static void write_profile_once(void)
{
    struct ppa_accel_profile profile;
    SceUID fd = -1;
    int ok = 0;

    if (g_accel.profile_write_attempted || !g_accel.profile_dirty ||
        g_accel.scalar_sample_ewma_us == 0U ||
        g_accel.vfpu_sample_ewma_us == 0U ||
        g_accel.calibration_samples < 3U ||
        g_accel.current_stage_mask == 0U)
        return;
    g_accel.profile_write_attempted = 1;

    memset(&profile, 0, sizeof(profile));
    profile.magic = PPA_ACCEL_PROFILE_MAGIC;
    profile.version = PPA_ACCEL_PROFILE_VERSION;
    profile.bytes = sizeof(profile);
    profile.hardware_model = (uint32_t)ppa_hardware_model();
    profile.build_id = profile_build_key();
    profile.scalar_sample_ewma_us = g_accel.scalar_sample_ewma_us;
    profile.vfpu_sample_ewma_us = g_accel.vfpu_sample_ewma_us;
    profile.stage_mask = g_accel.current_stage_mask;
    profile.preferred_plan = (uint32_t)g_accel.active_plan;
    profile.sample_count = g_accel.calibration_samples;
    profile.crc32 = profile_crc32(
        &profile,
        (unsigned int)offsetof(struct ppa_accel_profile, crc32));

    (void)sceIoMkdir("ms0:/PSP", 0777);
    (void)sceIoMkdir("ms0:/PSP/SYSTEM", 0777);
    (void)sceIoRemove(PPA_ACCEL_PROFILE_TEMP);
    fd = sceIoOpen(PPA_ACCEL_PROFILE_TEMP,
                   PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0)
        goto done;
    if (!write_exact(fd, &profile, sizeof(profile)))
        goto done;
    sceIoClose(fd);
    fd = -1;
    (void)sceIoRemove(PPA_ACCEL_PROFILE_PATH);
    if (sceIoRename(PPA_ACCEL_PROFILE_TEMP, PPA_ACCEL_PROFILE_PATH) < 0)
        goto done;
    ok = 1;

 done:
    if (fd >= 0)
        sceIoClose(fd);
    if (!ok)
        (void)sceIoRemove(PPA_ACCEL_PROFILE_TEMP);
    else
        g_accel.profile_dirty = 0;
}

static unsigned int current_cost_ratio_q8(void)
{
    unsigned int scalar_us = g_accel.scalar_frame_ewma_us != 0U ?
        g_accel.scalar_frame_ewma_us : g_accel.estimated_scalar_frame_us;
    unsigned int cpu = (unsigned int)scePowerGetCpuClockFrequencyInt();
    unsigned int bus = (unsigned int)scePowerGetBusClockFrequencyInt();
    uint64_t scalar_cost;
    uint64_t vfpu_cost;

    if (scalar_us == 0U || g_accel.vfpu_frame_ewma_us == 0U)
        return 0U;
    if (cpu == 0U) cpu = 1U;
    if (bus == 0U) bus = 1U;

    /* elapsed_us reflects CPU/VFPU occupancy.  Scratchpad row-transfer time is
     * charged again at half bus weight to reject "faster" plans that increase
     * memory/bus pressure enough to prevent a clock downshift. */
    scalar_cost = (uint64_t)scalar_us * cpu * 256ULL;
    vfpu_cost = (uint64_t)g_accel.vfpu_frame_ewma_us * cpu * 256ULL +
                (uint64_t)g_accel.transfer_ewma_us * bus * 128ULL;
    if (scalar_cost == 0ULL)
        return 0U;
    return (unsigned int)((vfpu_cost * 256ULL + scalar_cost / 2ULL) /
                          scalar_cost);
}

static void log_state(const char *event, const char *reason)
{
    (void)event;
    (void)reason;
}

static void set_plan(enum ppa_accel_plan plan, const char *reason)
{
    uint64_t now;
    if (g_accel.active_plan == plan)
        return;
    now = accel_now();
    g_accel.active_plan = plan;
    g_accel.last_change_us = now;
    ++g_accel.plan_changes;
    log_state("plan_change", reason);
}

void ppa_accel_governor_init(void)
{
    if (g_accel.initialized)
        return;
    memset(&g_accel, 0, sizeof(g_accel));
    g_accel.configured_mode = (enum ppa_accel_mode)PPA_ACCEL_DEFAULT_MODE;
    if (g_accel.configured_mode < PPA_ACCEL_MODE_AUTO ||
        g_accel.configured_mode > PPA_ACCEL_MODE_VFPU)
        g_accel.configured_mode = PPA_ACCEL_MODE_AUTO;
    g_accel.active_plan =
        g_accel.configured_mode == PPA_ACCEL_MODE_SCALAR ?
            PPA_ACCEL_PLAN_SCALAR_FALLBACK :
            PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR;
    (void)load_profile();
    if (g_accel.configured_mode == PPA_ACCEL_MODE_SCALAR)
        g_accel.active_plan = PPA_ACCEL_PLAN_SCALAR_FALLBACK;
    else if (g_accel.configured_mode == PPA_ACCEL_MODE_VFPU)
        g_accel.active_plan = PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR;
    g_accel.last_change_us = accel_now();
    g_accel.initialized = 1;
    log_state("init", g_accel.profile_loaded ? "profile_valid" :
                                              "default_plan");
}

void ppa_accel_governor_reset(void)
{
    struct ppa_accel_state prior;
    ppa_accel_governor_init();
    prior = g_accel;
    memset(&g_accel, 0, sizeof(g_accel));
    g_accel.configured_mode = prior.configured_mode;
    g_accel.active_plan = prior.active_plan;
    g_accel.scalar_sample_ewma_us = prior.scalar_sample_ewma_us;
    g_accel.vfpu_sample_ewma_us = prior.vfpu_sample_ewma_us;
    g_accel.calibration_samples = prior.calibration_samples;
    g_accel.current_stage_mask = prior.current_stage_mask;
    g_accel.profile_stage_mask = prior.profile_stage_mask;
    g_accel.pending_profile_scalar_us = prior.pending_profile_scalar_us;
    g_accel.pending_profile_vfpu_us = prior.pending_profile_vfpu_us;
    g_accel.pending_profile_plan = prior.pending_profile_plan;
    g_accel.profile_loaded = prior.profile_loaded;
    g_accel.profile_pending = prior.profile_pending;
    g_accel.profile_write_attempted = prior.profile_write_attempted;
    g_accel.profile_dirty = prior.profile_dirty;
    if (g_accel.configured_mode == PPA_ACCEL_MODE_SCALAR)
        g_accel.active_plan = PPA_ACCEL_PLAN_SCALAR_FALLBACK;
    else if (g_accel.configured_mode == PPA_ACCEL_MODE_VFPU)
        g_accel.active_plan = PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR;
    g_accel.last_change_us = accel_now();
    g_accel.initialized = 1;
    log_state("session_reset", "playback_start");
}

void ppa_accel_governor_set_mode(enum ppa_accel_mode mode)
{
    ppa_accel_governor_init();
    if (mode < PPA_ACCEL_MODE_AUTO || mode > PPA_ACCEL_MODE_VFPU)
        mode = PPA_ACCEL_MODE_AUTO;
    if (g_accel.configured_mode == mode)
        return;
    g_accel.configured_mode = mode;
    if (mode == PPA_ACCEL_MODE_SCALAR)
        set_plan(PPA_ACCEL_PLAN_SCALAR_FALLBACK, "forced_scalar");
    else if (mode == PPA_ACCEL_MODE_VFPU)
        set_plan(PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR, "forced_vfpu");
    else
        log_state("mode_change", "auto");
}

static void adopt_or_reject_pending_profile(unsigned int stage_mask)
{
    if (!g_accel.profile_pending)
        return;
    g_accel.profile_pending = 0;
    if (stage_mask == g_accel.profile_stage_mask) {
        g_accel.scalar_sample_ewma_us =
            g_accel.pending_profile_scalar_us;
        g_accel.vfpu_sample_ewma_us =
            g_accel.pending_profile_vfpu_us;
        if (g_accel.configured_mode == PPA_ACCEL_MODE_AUTO) {
            g_accel.active_plan = g_accel.pending_profile_plan;
            g_accel.last_change_us = accel_now();
            if (g_accel.active_plan == PPA_ACCEL_PLAN_SCALAR_FALLBACK)
                g_accel.next_probe_us =
                    g_accel.last_change_us + PPA_ACCEL_REPROBE_US;
        }
        log_state("profile_apply", "stage_output_match");
    } else {
        g_accel.profile_loaded = 0;
        g_accel.scalar_sample_ewma_us = 0U;
        g_accel.vfpu_sample_ewma_us = 0U;
        g_accel.calibration_samples = 0U;
        if (g_accel.configured_mode == PPA_ACCEL_MODE_AUTO)
            g_accel.active_plan = PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR;
        log_state("profile_reject", "stage_output_mismatch");
    }
}

enum ppa_accel_plan ppa_accel_governor_plan(unsigned int stage_mask,
                                            unsigned int pixel_count,
                                            unsigned int frame_number)
{
    uint64_t now;
    (void)frame_number;
    ppa_accel_governor_init();
    ++g_accel.decisions;

    if (stage_mask != 0U) {
        if (g_accel.current_stage_mask == 0U) {
            g_accel.current_stage_mask = stage_mask;
            adopt_or_reject_pending_profile(stage_mask);
        } else if (g_accel.current_stage_mask != stage_mask) {
            g_accel.current_stage_mask = stage_mask;
            g_accel.scalar_frame_ewma_us = 0U;
            g_accel.vfpu_frame_ewma_us = 0U;
            g_accel.transfer_ewma_us = 0U;
            g_accel.scalar_sample_ewma_us = 0U;
            g_accel.vfpu_sample_ewma_us = 0U;
            g_accel.estimated_scalar_frame_us = 0U;
            g_accel.calibration_samples = 0U;
            g_accel.active_plan = PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR;
            log_state("stage_change", "recalibrate_output_plan");
        }
    }

    if (stage_mask == 0U || pixel_count < 64U) {
        ++g_accel.scalar_frames;
        return PPA_ACCEL_PLAN_SCALAR_FALLBACK;
    }
#if !PPA_ENABLE_CPU_FULL_FRAME_FILTERS
    ++g_accel.vfpu_frames;
    return PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR;
#endif
    if (g_accel.configured_mode == PPA_ACCEL_MODE_SCALAR) {
        ++g_accel.scalar_frames;
        return PPA_ACCEL_PLAN_SCALAR_FALLBACK;
    }
    if (g_accel.configured_mode == PPA_ACCEL_MODE_VFPU) {
        ++g_accel.vfpu_frames;
        return PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR;
    }
#if !PPA_ENABLE_ACCEL_GOVERNOR
    ++g_accel.vfpu_frames;
    return PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR;
#else
    now = accel_now();
    if (g_accel.active_plan == PPA_ACCEL_PLAN_SCALAR_FALLBACK &&
        now >= g_accel.next_probe_us) {
        g_accel.probe_frame = 1;
        ++g_accel.vfpu_frames;
        return PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR;
    }
    if (g_accel.active_plan == PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR)
        ++g_accel.vfpu_frames;
    else
        ++g_accel.scalar_frames;
    return g_accel.active_plan;
#endif
}

void ppa_accel_governor_observe_vfpu_frame(unsigned int elapsed_us,
                                           unsigned int transfer_us,
                                           unsigned int pixel_count)
{
    (void)pixel_count;
    ppa_accel_governor_init();
    g_accel.vfpu_frame_ewma_us = ewma8(g_accel.vfpu_frame_ewma_us,
                                       elapsed_us);
    if (transfer_us != 0U)
        g_accel.transfer_ewma_us =
            ewma8(g_accel.transfer_ewma_us, transfer_us);
    
}

void ppa_accel_governor_observe_scalar_fallback(unsigned int elapsed_us,
                                                unsigned int pixel_count)
{
    (void)pixel_count;
    ppa_accel_governor_init();
    g_accel.scalar_frame_ewma_us = ewma8(g_accel.scalar_frame_ewma_us,
                                         elapsed_us);
    g_accel.estimated_scalar_frame_us = g_accel.scalar_frame_ewma_us;
}

void ppa_accel_governor_observe_sample(unsigned int scalar_us,
                                       unsigned int vfpu_us,
                                       unsigned int max_channel_delta,
                                       int verification_failed)
{
    uint64_t now;
    unsigned int ratio_q8;
    ppa_accel_governor_init();
    if (scalar_us == 0U || vfpu_us == 0U)
        return;

    g_accel.scalar_sample_ewma_us =
        ewma8(g_accel.scalar_sample_ewma_us, scalar_us);
    g_accel.vfpu_sample_ewma_us =
        ewma8(g_accel.vfpu_sample_ewma_us, vfpu_us);
    ++g_accel.calibration_samples;

    if (verification_failed) {
        ++g_accel.verification_failures;
    } else {
    }

    if (g_accel.vfpu_sample_ewma_us != 0U &&
        g_accel.vfpu_frame_ewma_us != 0U &&
        g_accel.scalar_frame_ewma_us == 0U) {
        g_accel.estimated_scalar_frame_us = (unsigned int)(
            ((uint64_t)g_accel.vfpu_frame_ewma_us *
             g_accel.scalar_sample_ewma_us +
             g_accel.vfpu_sample_ewma_us / 2U) /
            g_accel.vfpu_sample_ewma_us);
    }

    ratio_q8 = current_cost_ratio_q8();
    if (ratio_q8 == 0U) {
        ratio_q8 = (unsigned int)(((uint64_t)vfpu_us * 256ULL +
                                   scalar_us / 2U) / scalar_us);
    }
    g_accel.last_cost_ratio_q8 = ratio_q8;

    if (!verification_failed && ratio_q8 <= PPA_ACCEL_GOOD_MARGIN_Q8) {
        ++g_accel.good_windows;
        g_accel.bad_windows = 0;
    } else if (verification_failed || ratio_q8 >= PPA_ACCEL_BAD_MARGIN_Q8) {
        ++g_accel.bad_windows;
        g_accel.good_windows = 0;
    } else {
        g_accel.good_windows = 0;
        g_accel.bad_windows = 0;
    }

    now = accel_now();
    if (g_accel.configured_mode == PPA_ACCEL_MODE_AUTO) {
        if (now - g_accel.last_change_us >= PPA_ACCEL_MIN_PLAN_HOLD_US ||
            verification_failed) {
            if (verification_failed || g_accel.bad_windows >= 3U) {
                set_plan(PPA_ACCEL_PLAN_SCALAR_FALLBACK,
                         verification_failed ? "verification_failed" :
                                               "energy_cost_higher");
                g_accel.next_probe_us = now + PPA_ACCEL_REPROBE_US;
                g_accel.probe_frame = 0;
                g_accel.bad_windows = 0;
            } else if (g_accel.good_windows >= 2U ||
                       (g_accel.probe_frame &&
                        ratio_q8 <= PPA_ACCEL_GOOD_MARGIN_Q8)) {
                set_plan(PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR,
                         "energy_cost_lower");
                g_accel.probe_frame = 0;
                g_accel.good_windows = 0;
            } else if (g_accel.probe_frame) {
                g_accel.probe_frame = 0;
                g_accel.next_probe_us = now + PPA_ACCEL_REPROBE_US;
            }
        }
    }
    if (g_accel.calibration_samples >= 3U)
        g_accel.profile_dirty = 1;
}

void ppa_accel_governor_flush_profile(void)
{
    ppa_accel_governor_init();
    write_profile_once();
}

int ppa_accel_governor_calibration_due(unsigned int frame_number)
{
    ppa_accel_governor_init();
    if (g_accel.probe_frame)
        return 1;
    /* Three sparse startup samples establish a stable decision.  Once settled,
     * a tiny sample every ~10 seconds tracks clock/output changes without
     * duplicating full-frame work. */
    if (g_accel.calibration_samples < 3U)
        return frame_number != 0U && (frame_number % 30U) == 0U;
    return frame_number != 0U && (frame_number % 240U) == 0U;
}

void ppa_accel_governor_snapshot(struct ppa_accel_snapshot *out)
{
    if (out == 0)
        return;
    ppa_accel_governor_init();
    memset(out, 0, sizeof(*out));
    out->configured_mode = g_accel.configured_mode;
    out->active_plan = g_accel.active_plan;
    out->vfpu_frame_ewma_us = g_accel.vfpu_frame_ewma_us;
    out->scalar_frame_ewma_us = g_accel.scalar_frame_ewma_us;
    out->transfer_ewma_us = g_accel.transfer_ewma_us;
    out->scalar_sample_ewma_us = g_accel.scalar_sample_ewma_us;
    out->vfpu_sample_ewma_us = g_accel.vfpu_sample_ewma_us;
    out->estimated_scalar_frame_us = g_accel.estimated_scalar_frame_us;
    out->last_cost_ratio_q8 = g_accel.last_cost_ratio_q8;
    out->decisions = g_accel.decisions;
    out->plan_changes = g_accel.plan_changes;
    out->vfpu_frames = g_accel.vfpu_frames;
    out->scalar_frames = g_accel.scalar_frames;
    out->verification_failures = g_accel.verification_failures;
    out->profile_loaded = g_accel.profile_loaded;
}
