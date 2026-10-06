#ifndef PPA_ACCEL_GOVERNOR_H
#define PPA_ACCEL_GOVERNOR_H

#ifdef __cplusplus
extern "C" {
#endif

enum ppa_accel_mode {
    PPA_ACCEL_MODE_AUTO = 0,
    PPA_ACCEL_MODE_SCALAR = 1,
    PPA_ACCEL_MODE_VFPU = 2
};

enum ppa_accel_plan {
    PPA_ACCEL_PLAN_SCALAR_FALLBACK = 0,
    PPA_ACCEL_PLAN_VFPU_DIRECT_COLOR = 1
};

struct ppa_accel_snapshot {
    enum ppa_accel_mode configured_mode;
    enum ppa_accel_plan active_plan;
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
    unsigned int verification_failures;
    int profile_loaded;
};

void ppa_accel_governor_init(void);
void ppa_accel_governor_reset(void);
void ppa_accel_governor_set_mode(enum ppa_accel_mode mode);
enum ppa_accel_plan ppa_accel_governor_plan(unsigned int stage_mask,
                                            unsigned int pixel_count,
                                            unsigned int frame_number);
void ppa_accel_governor_observe_vfpu_frame(unsigned int elapsed_us,
                                           unsigned int transfer_us,
                                           unsigned int pixel_count);
void ppa_accel_governor_observe_scalar_fallback(unsigned int elapsed_us,
                                                unsigned int pixel_count);
void ppa_accel_governor_observe_sample(unsigned int scalar_us,
                                       unsigned int vfpu_us,
                                       unsigned int max_channel_delta,
                                       int verification_failed);
void ppa_accel_governor_flush_profile(void);
int ppa_accel_governor_calibration_due(unsigned int frame_number);
void ppa_accel_governor_snapshot(struct ppa_accel_snapshot *out);

#ifdef __cplusplus
}
#endif

#endif
