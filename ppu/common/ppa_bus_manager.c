#include "ppa_bus_manager.h"

#include <pspkernel.h>
#include <string.h>

struct ppa_bus_state {
    volatile unsigned int postprocess_us;
    volatile unsigned int cache_us;
    volatile unsigned int cache_bytes;
    volatile unsigned int cpu_pressure_hint_q8;
};

static struct ppa_bus_state g_bus;

static unsigned int clamp_q8(unsigned int value)
{
    return value > 255U ? 255U : value;
}

static unsigned int filtered_pressure(unsigned int previous,
                                      unsigned int sample)
{
    /* Fast rise, slow fall.  A genuine deadline risk blocks a downshift in the
     * current frame, while a transient device wait decays without oscillation. */
    if (sample > previous)
        return (previous + sample + 1U) >> 1;
    return (previous * 7U + sample) >> 3;
}

void ppa_bus_manager_init(void)
{
    ppa_bus_manager_reset();
}

void ppa_bus_manager_reset(void)
{
    memset(&g_bus, 0, sizeof(g_bus));
}

struct ppa_bus_token ppa_bus_begin(enum ppa_bus_domain domain)
{
    struct ppa_bus_token token;

    token.start_us = 0;
    token.domain = (unsigned int)domain;
    token.active = 0;

    /* Storage, Sony decode, GE, display and audio calls include device waits.
     * None feeds a live policy. Do not timestamp or atomically count them on
     * every burst/frame just to produce an unread diagnostic score. */
    if (domain != PPA_BUS_POSTPROCESS && domain != PPA_BUS_CACHE)
        return token;
    token.start_us = sceKernelGetSystemTimeLow();
    token.active = 1;
    return token;
}

void ppa_bus_end(struct ppa_bus_token *token)
{
    unsigned int now;
    unsigned int elapsed;
    if (token == 0 || !token->active)
        return;
    token->active = 0;

    now = sceKernelGetSystemTimeLow();
    elapsed = now - token->start_us;
    if (token->domain == PPA_BUS_POSTPROCESS)
        __sync_fetch_and_add(&g_bus.postprocess_us, elapsed);
    else if (token->domain == PPA_BUS_CACHE)
        __sync_fetch_and_add(&g_bus.cache_us, elapsed);
}

void ppa_bus_note_cache_bytes(unsigned int bytes)
{
    __sync_fetch_and_add(&g_bus.cache_bytes, bytes);
}

void ppa_bus_frame_pulse(unsigned int frame_budget_us, int late_frame)
{
    unsigned int cpu_critical_us;
    unsigned int cache_bytes;
    unsigned int pressure_hint;

    if (frame_budget_us < 8000U)
        frame_budget_us = 8000U;
    if (frame_budget_us > 100000U)
        frame_budget_us = 100000U;

    cpu_critical_us = __sync_lock_test_and_set(&g_bus.postprocess_us, 0U) +
                      __sync_lock_test_and_set(&g_bus.cache_us, 0U);
    cache_bytes = __sync_lock_test_and_set(&g_bus.cache_bytes, 0U);

    pressure_hint = (unsigned int)(
        ((unsigned long long)cpu_critical_us * 192ULL) /
        (unsigned long long)frame_budget_us);
    pressure_hint += cache_bytes >> 18; /* 1 point per 256 KiB */
    if (late_frame)
        pressure_hint += 48U;
    pressure_hint = clamp_q8(pressure_hint);

    g_bus.cpu_pressure_hint_q8 = filtered_pressure(
        g_bus.cpu_pressure_hint_q8, pressure_hint);
}

unsigned int ppa_bus_cpu_pressure_hint_q8(void)
{
    return g_bus.cpu_pressure_hint_q8;
}

unsigned int ppa_bus_pressure_q8(void)
{
    return ppa_bus_cpu_pressure_hint_q8();
}
