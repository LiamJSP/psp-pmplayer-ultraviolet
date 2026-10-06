#include "ppa_seek_controller.h"

#include <limits.h>
#include <stdio.h>
#include <pspkernel.h>

static unsigned int seek_clamp_u32(uint64_t value)
{
    return value > 0xffffffffULL ? 0xffffffffU : (unsigned int)value;
}

void ppa_seek_controller_reset(struct ppa_seek_controller *controller)
{
    if (controller == 0)
        return;
    controller->signed_level = 0;
    controller->cursor_ms = 0;
    controller->next_step_ms = 0U;
    controller->allow_frame_drop = 0U;
    controller->cursor_valid = 0U;
}

int ppa_seek_controller_press(struct ppa_seek_controller *controller,
                              int direction,
                              uint64_t now_us)
{
    int level;

    if (controller == 0 || (direction != -1 && direction != 1))
        return 0;

    (void)now_us;
    /* A fixed seek is a single edge-triggered transaction, never a temporary
     * fast-forward/rewind state.  Return the direction to the caller while
     * keeping active()==false immediately after the edge. */
    level = direction;
    controller->signed_level = 0;
    controller->cursor_ms = 0;
    controller->cursor_valid = 0U;
    controller->allow_frame_drop = 0U;
    controller->next_step_ms = 0U;
    return level;
}

int ppa_seek_controller_take_due_step(struct ppa_seek_controller *controller,
                                      uint64_t now_us)
{
    (void)controller;
    (void)now_us;
    return 0;
}

void ppa_seek_controller_ack_step(struct ppa_seek_controller *controller,
                                  uint64_t now_us)
{
    (void)controller;
    (void)now_us;
}

int ppa_seek_controller_active(const struct ppa_seek_controller *controller)
{
    return controller != 0 && controller->signed_level != 0;
}

int ppa_seek_controller_signed_level(const struct ppa_seek_controller *controller)
{
    return controller == 0 ? 0 : controller->signed_level;
}

int ppa_seek_controller_frame_drop_allowed(const struct ppa_seek_controller *controller)
{
    return controller != 0 && controller->signed_level != 0 &&
           controller->allow_frame_drop != 0U;
}

unsigned int ppa_seek_controller_jump_ms(unsigned int level,
                                         unsigned int duration_ms)
{
    uint64_t base_ms;
    uint64_t jump_ms;

    (void)level;
    /* One tap traverses about 1/120th of the programme. Five seconds keeps
     * short clips controllable; thirty seconds keeps feature films useful.
     * SEEK_JUMP_MULTIPLIER is a fixed-point percentage configured in the
     * Makefile (100=1.00x, 50=0.50x, 200=2.00x). */
    if (duration_ms == 0U)
        base_ms = 10000ULL;
    else {
        base_ms = ((uint64_t)duration_ms + 119ULL) / 120ULL;
        if (base_ms < 5000ULL)
            base_ms = 5000ULL;
        if (base_ms > 30000ULL)
            base_ms = 30000ULL;
    }
    jump_ms = (base_ms * (uint64_t)(SEEK_JUMP_MULTIPLIER > 0 ?
                                    SEEK_JUMP_MULTIPLIER : 1) + 50ULL) / 100ULL;
    if (jump_ms < 250ULL)
        jump_ms = 250ULL;
    if (duration_ms != 0U && jump_ms > duration_ms)
        jump_ms = duration_ms;
    return seek_clamp_u32(jump_ms);
}

int ppa_seek_controller_clamp_target_ms(int64_t target,
                                        unsigned int duration_ms)
{
    if (target < 0)
        target = 0;
    if (duration_ms != 0U) {
        uint64_t tail_guard = duration_ms < 30000U ? 250U : 1000U;
        int64_t last_valid_ms = duration_ms > tail_guard ?
            (int64_t)duration_ms - (int64_t)tail_guard : 0;
        if (target > last_valid_ms)
            target = last_valid_ms;
    }
    if (target > INT_MAX)
        target = INT_MAX;
    return (int)target;
}

int ppa_seek_controller_target_ms(int current_ms,
                                  int signed_level,
                                  unsigned int duration_ms)
{
    int direction = signed_level < 0 ? -1 : 1;
    unsigned int jump;
    int64_t target;
    if (signed_level == 0)
        return current_ms < 0 ? 0 : current_ms;
    jump = ppa_seek_controller_jump_ms(1U, duration_ms);
    target = (int64_t)(current_ms < 0 ? 0 : current_ms) +
             (int64_t)direction * jump;
    return ppa_seek_controller_clamp_target_ms(target, duration_ms);
}

int ppa_seek_controller_next_target_ms(struct ppa_seek_controller *controller,
                                       int observed_current_ms,
                                       unsigned int duration_ms)
{
    int base_ms;
    int target_ms;

    if (controller == 0)
        return observed_current_ms < 0 ? 0 : observed_current_ms;

    base_ms = controller->cursor_valid ? controller->cursor_ms :
              (observed_current_ms < 0 ? 0 : observed_current_ms);
    target_ms = ppa_seek_controller_target_ms(base_ms,
                                               controller->signed_level,
                                               duration_ms);
    controller->cursor_ms = target_ms;
    controller->cursor_valid = 1U;
    return target_ms;
}

int ppa_seek_controller_cursor_ms(const struct ppa_seek_controller *controller,
                                  int fallback_ms)
{
    if (controller == 0 || controller->cursor_valid == 0U)
        return fallback_ms < 0 ? 0 : fallback_ms;
    return controller->cursor_ms;
}

void ppa_seek_controller_format_label(char *out,
                                      unsigned int out_size,
                                      int signed_level,
                                      unsigned int duration_ms)
{
    unsigned int level;
    unsigned int jump;

    if (out == 0 || out_size == 0U)
        return;
    level = signed_level < 0 ? (unsigned int)(-(int64_t)signed_level) :
                              (unsigned int)signed_level;
    if (level < PPA_SEEK_LEVEL_MIN)
        level = PPA_SEEK_LEVEL_MIN;
    if (level > PPA_SEEK_LEVEL_MAX)
        level = PPA_SEEK_LEVEL_MAX;
    jump = ppa_seek_controller_jump_ms(level, duration_ms);

    if (jump < 1000U)
        snprintf(out, out_size, "%u ms", jump);
    else
        snprintf(out, out_size, "%u.%u sec", jump / 1000U,
                 (jump % 1000U) / 100U);
}

static int ppu_seek_request_lock(struct ppu_seek_request *r)
{
    return r && r->lock >= 0 && sceKernelWaitSema(r->lock, 1, 0) >= 0;
}
int ppu_seek_request_init(struct ppu_seek_request *r)
{
    if (!r) return 0;
    r->pending = r->cursor_valid = 0U;
    r->target_ms = 0;
    r->selected_ms = r->held_mask = 0U;
    r->lock = sceKernelCreateSema("ppu_seek_request", 0, 1, 1, 0);
    return r->lock >= 0;
}
void ppu_seek_request_destroy(struct ppu_seek_request *r)
{
    if (r && r->lock >= 0) sceKernelDeleteSema(r->lock);
    if (r) r->lock = -1;
}
void ppu_seek_request_reset(struct ppu_seek_request *r)
{
    if (!ppu_seek_request_lock(r)) return;
    r->pending = r->cursor_valid = 0U;
    sceKernelSignalSema(r->lock, 1);
}
int ppu_seek_request_cursor(struct ppu_seek_request *r, int fallback_ms,
                            int in_flight, uint32_t now_ms)
{
    int target = fallback_ms < 0 ? 0 : fallback_ms;
    if (!ppu_seek_request_lock(r)) return target;
    if (r->cursor_valid && (r->pending || in_flight ||
        (uint32_t)(now_ms - r->selected_ms) < 750U)) target = r->target_ms;
    sceKernelSignalSema(r->lock, 1);
    return target;
}
void ppu_seek_request_submit(struct ppu_seek_request *r, int target_ms,
                             uint32_t now_ms, uint32_t held_mask)
{
    if (!ppu_seek_request_lock(r)) return;
    r->target_ms = target_ms < 0 ? 0 : target_ms;
    r->pending = r->cursor_valid = 1U;
    r->selected_ms = now_ms;
    r->held_mask = held_mask;
    sceKernelSignalSema(r->lock, 1);
}
int ppu_seek_request_take(struct ppu_seek_request *r, uint32_t now_ms,
                          uint32_t held_buttons)
{
    int target = -1;
    if (!ppu_seek_request_lock(r)) return target;
    if (r->pending && (r->held_mask == 0U ||
        (!(held_buttons & r->held_mask) &&
         (uint32_t)(now_ms - r->selected_ms) >= PPU_SEEK_QUIET_MS))) {
        target = r->target_ms;
        r->pending = 0U;
    }
    sceKernelSignalSema(r->lock, 1);
    return target;
}
