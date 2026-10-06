#ifndef PPA_SEEK_CONTROLLER_H
#define PPA_SEEK_CONTROLLER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UI-owned selection -> producer-owned seek mailbox. Only the newest target
 * is retained. The semaphore protects all fields; no shared 64-bit deadline.
 * Decoder/display epoch ownership remains in the container adapter. A zero
 * held_mask commits direction edges/held ticks immediately; nonzero masks
 * retain chapter release/quiet-window coalescing. */
#define PPU_SEEK_QUIET_MS 300U
struct ppu_seek_request {
    int lock;
    int target_ms;
    unsigned int pending;
    unsigned int cursor_valid;
    uint32_t selected_ms;
    uint32_t held_mask;
};
int ppu_seek_request_init(struct ppu_seek_request *request);
void ppu_seek_request_destroy(struct ppu_seek_request *request);
void ppu_seek_request_reset(struct ppu_seek_request *request);
int ppu_seek_request_cursor(struct ppu_seek_request *request, int fallback_ms,
                            int in_flight, uint32_t now_ms);
void ppu_seek_request_submit(struct ppu_seek_request *request, int target_ms,
                             uint32_t now_ms, uint32_t held_mask);
int ppu_seek_request_take(struct ppu_seek_request *request, uint32_t now_ms,
                          uint32_t held_buttons);

#define PPA_SEEK_LEVEL_MIN 1
#define PPA_SEEK_LEVEL_MAX 4
#define PPA_SEEK_STEP_INTERVAL_US 250000ULL

#ifndef SEEK_JUMP_MULTIPLIER
#define SEEK_JUMP_MULTIPLIER 100
#endif

/*
 * Shared seek controller.
 *
 * The active input path uses one fixed duration-scaled jump for a direction
 * down event and each fixed 4 Hz held tick from ctrl.c. It publishes that
 * target through ppu_seek_request. No sticky trick-play mode is enabled.
 * Legacy controller accessors below remain for the container fallback state.
 *
 * cursor_ms is an independent media-time cursor. It is intentionally not
 * derived again from the most recently decoded/displayed frame after every
 * seek, because container seeks commonly restart decoding at an earlier
 * keyframe. Re-basing every tick on that preroll timestamp creates a small
 * repeated GOP loop instead of continuous trick-play progression.
 */
struct ppa_seek_controller {
    int signed_level;
    int cursor_ms;
    uint32_t next_step_ms;
    unsigned int allow_frame_drop;
    unsigned int cursor_valid;
};

void ppa_seek_controller_reset(struct ppa_seek_controller *controller);

/* Legacy one-shot edge helper; does not leave the controller active. */
int ppa_seek_controller_press(struct ppa_seek_controller *controller,
                              int direction,
                              uint64_t now_us);

/* Legacy continuous-mode entry point; currently always returns zero. */
int ppa_seek_controller_take_due_step(struct ppa_seek_controller *controller,
                                      uint64_t now_us);

/* Legacy continuous-mode acknowledgement; currently a no-op. */
void ppa_seek_controller_ack_step(struct ppa_seek_controller *controller,
                                  uint64_t now_us);

int ppa_seek_controller_active(const struct ppa_seek_controller *controller);
int ppa_seek_controller_signed_level(const struct ppa_seek_controller *controller);
int ppa_seek_controller_frame_drop_allowed(const struct ppa_seek_controller *controller);

/* Duration-aware media-time displacement, using one fixed jump tier. */
unsigned int ppa_seek_controller_jump_ms(unsigned int level,
                                         unsigned int duration_ms);

/* Stateless clamp helper retained for chapter/tests and callers that do not
 * participate in a sticky trick-play session. */
int ppa_seek_controller_target_ms(int current_ms,
                                  int signed_level,
                                  unsigned int duration_ms);
/* Shared signed/EOF-safe clamp for direction and absolute chapter targets. */
int ppa_seek_controller_clamp_target_ms(int64_t target_ms,
                                        unsigned int duration_ms);

/* Advances and returns the sticky virtual cursor. observed_current_ms is used
 * only once, when the session has no cursor yet; later decoded preroll frames
 * cannot drag the cursor back to the preceding keyframe. */
int ppa_seek_controller_next_target_ms(struct ppa_seek_controller *controller,
                                       int observed_current_ms,
                                       unsigned int duration_ms);

int ppa_seek_controller_cursor_ms(const struct ppa_seek_controller *controller,
                                  int fallback_ms);

void ppa_seek_controller_format_label(char *out,
                                      unsigned int out_size,
                                      int signed_level,
                                      unsigned int duration_ms);

#ifdef __cplusplus
}
#endif

#endif
