/* Private adapter for the pinned libsamplerate 0.2.2 LINEAR_DATA/SRC_STATE.
 * Included only after src_linear.c by audio_src_linear.c. See
 * docs/AUDIO_PERFORMANCE_2026-10-06.md before changing the upstream version.
 *
 * PPU uses constant integer input rates and 44100-Hz stereo output. Track
 * phase in exact 1/44100-source-frame ticks; perform interpolation with the
 * Allegrex scalar single-precision FPU. No double operations, division,
 * allocation, or calls to rounding functions occur inside the sample loop.
 * History, consumed counts and EOF guards retain the streaming contract.
 */
#ifndef PPU_AUDIO_SRC_LINEAR_PSP_H
#define PPU_AUDIO_SRC_LINEAR_PSP_H

static SRC_ERROR ppu_linear_const_process(SRC_STATE *state, SRC_DATA *data)
{
    LINEAR_DATA *priv = (LINEAR_DATA *)state->private_data;
    double rate_value, phase_value;
    unsigned int rate, phase;
    long used = 0, generated = 0;
    long rate_integer, phase_integer;

    /* Keep generic/variable-ratio users on upstream code. Bounds also make
     * the integer casts and frame*2 indexing below independent of input size. */
    if (state->channels != 2 || !(data->src_ratio >= 44100.0 / 48000.0 &&
          data->src_ratio <= 44100.0 / 8000.0) ||
        !(state->last_position >= 0.0 && state->last_position <= 2.0) ||
        data->input_frames > 65536 || data->output_frames > 65536)
        return linear_vari_process(state, data);
    if (!priv) return SRC_ERR_NO_PRIVATE;
    if (data->input_frames <= 0 || data->output_frames <= 0)
        return SRC_ERR_NO_ERROR;

    rate_value = 44100.0 / data->src_ratio;
    phase_value = state->last_position * 44100.0;
    /* Nonnegative bounded values: explicit nearest rounding does not depend
     * on another component's current floating-point rounding mode. */
    rate_integer = (long)(rate_value + 0.5);
    phase_integer = (long)(phase_value + 0.5);
    if (rate_integer < 8000 || rate_integer > 48000 ||
        fabs(rate_value - rate_integer) > 1e-7 ||
        fabs(phase_value - phase_integer) > 1e-7)
        return linear_vari_process(state, data);
    rate = (unsigned int)rate_integer;
    phase = (unsigned int)phase_integer;

    if (!priv->dirty) {
        priv->last_value[0] = data->data_in[0];
        priv->last_value[1] = data->data_in[1];
        priv->dirty = true;
    }
    /* First interpolate between retained history and this buffer's first
     * frame. One input frame is sufficient here; never index before data_in. */
    while (phase < 44100U && generated < data->output_frames) {
        float fraction = (float)phase * (1.0f / 44100.0f);
        data->data_out[generated * 2] = priv->last_value[0] + fraction *
            (data->data_in[0] - priv->last_value[0]);
        data->data_out[generated * 2 + 1] = priv->last_value[1] + fraction *
            (data->data_in[1] - priv->last_value[1]);
        ++generated;
        phase += rate;
    }
    used = (long)(phase / 44100U);
    phase %= 44100U;

    while (generated < data->output_frames && used < data->input_frames) {
        const float *a = data->data_in + (used - 1) * 2;
        const float *b = a + 2;
        float fraction = (float)phase * (1.0f / 44100.0f);
        data->data_out[generated * 2] = a[0] + fraction * (b[0] - a[0]);
        data->data_out[generated * 2 + 1] = a[1] + fraction * (b[1] - a[1]);
        ++generated;
        phase += rate;
        /* rate <= 48000: at most two source frames advance per output. */
        if (phase >= 44100U) { phase -= 44100U; ++used; }
        if (phase >= 44100U) { phase -= 44100U; ++used; }
    }
    if (used > data->input_frames) {
        phase += (unsigned int)(used - data->input_frames) * 44100U;
        used = data->input_frames;
    }
    if (used) {
        priv->last_value[0] = data->data_in[(used - 1) * 2];
        priv->last_value[1] = data->data_in[(used - 1) * 2 + 1];
    }
    state->last_position = (double)phase / 44100.0;
    state->last_ratio = data->src_ratio;
    data->input_frames_used = used;
    data->output_frames_gen = generated;
    return SRC_ERR_NO_ERROR;
}

static SRC_STATE_VT ppu_linear_state_vt = {
    linear_vari_process, ppu_linear_const_process,
    linear_reset, linear_copy, linear_close
};

SRC_STATE *linear_state_new(int channels, SRC_ERROR *error)
{
    SRC_STATE *state = ppu_linear_state_new_upstream(channels, error);
    if (state) state->vt = &ppu_linear_state_vt;
    return state;
}
#endif
