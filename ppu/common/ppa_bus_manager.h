#ifndef PPA_BUS_MANAGER_H
#define PPA_BUS_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Only POSTPROCESS and CACHE are timed for the CPU governor. Other category
 * call sites remain compatible but their device waits are not sampled. */
enum ppa_bus_domain {
    PPA_BUS_STORAGE = 0,
    PPA_BUS_DECODE,
    PPA_BUS_POSTPROCESS,
    PPA_BUS_GE,
    PPA_BUS_DISPLAY,
    PPA_BUS_AUDIO_OUTPUT_WAIT,
    PPA_BUS_CACHE,
    PPA_BUS_DOMAIN_COUNT
};

/* Preserve the historic domain number and callers outside this tree. This
 * domain measures sceAudioOutputBlocking, never SC decode/resample work. */
#define PPA_BUS_AUDIO PPA_BUS_AUDIO_OUTPUT_WAIT

struct ppa_bus_token {
    unsigned int start_us;
    unsigned int domain;
    unsigned int active;
};

void ppa_bus_manager_init(void);
void ppa_bus_manager_reset(void);
struct ppa_bus_token ppa_bus_begin(enum ppa_bus_domain domain);
void ppa_bus_end(struct ppa_bus_token *token);
void ppa_bus_note_cache_bytes(unsigned int bytes);
void ppa_bus_frame_pulse(unsigned int frame_budget_us, int late_frame);

/* Governor hint from timed postprocess/cache work, cache bytes and a late
 * frame flag. This is not total Allegrex utilization or device wait time. */
unsigned int ppa_bus_cpu_pressure_hint_q8(void);

/* Compatibility alias for the same governor hint. */
unsigned int ppa_bus_pressure_q8(void);

#ifdef __cplusplus
}
#endif

#endif
