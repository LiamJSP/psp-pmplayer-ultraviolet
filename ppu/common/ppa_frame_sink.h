#ifndef PPA_FRAME_SINK_H
#define PPA_FRAME_SINK_H

#include <pspkernel.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ppa_frame_sink_reset_timing(void);

/* Presenter-owned FIFO lease. Decoding owns a ring slot until it is queued;
 * the presenter retains that credit while the slot is scanned or skipped
 * behind scanout. Only a completed next-frame swap retires the previous batch.
 * The descriptor is thread-local: the semaphore publishes reuse to decode. */
struct ppa_frame_lease {
    unsigned int retained_credits;
    const void *scanout;
    unsigned int epoch;
    int64_t timestamp_ms;
};
void ppa_frame_lease_init(struct ppa_frame_lease *lease);
void ppa_frame_lease_skip(struct ppa_frame_lease *lease);
int ppa_frame_lease_complete_swap(struct ppa_frame_lease *lease,
                                 SceUID producer_credits, const void *scanout,
                                 unsigned int epoch, int64_t timestamp_ms);
int ppa_frame_surface_equal(const void *a, const void *b);

/* Returns a robust duration estimate for AV-sync tolerance.  Real adjacent PTS
 * is preferred; nominal duration is used across seeks/discontinuities. */
int ppa_frame_sink_effective_duration_ms(int64_t previous_timestamp_ms,
                                         int64_t timestamp_ms,
                                         int nominal_duration_ms);

int ppa_frame_sink_display_timed(void *frame_buffer,
                                 int texture_width,
                                 int pixel_format,
                                 int setbuf_mode,
                                 int64_t media_timestamp_ms,
                                 int nominal_duration_ms);

/* Compatibility wrapper for call sites without timestamps. */
int ppa_frame_sink_display(void *frame_buffer,
                           int texture_width,
                           int pixel_format,
                           int setbuf_mode);

#ifdef __cplusplus
}
#endif

#endif
