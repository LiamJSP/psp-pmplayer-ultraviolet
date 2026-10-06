#include "audio_src_config.h"
/* Keep the vendored core intact. s16 / 32768 is exactly representable in
 * binary32; an intermediate double adds no precision for this conversion. */
#define src_short_to_float_array ppu_src_short_to_float_upstream
#include "../../third_party/libsamplerate-0.2.2/src/samplerate.c"
#undef src_short_to_float_array

void src_short_to_float_array(const short *in, float *out, int len)
{
    int i;
    for (i = 0; i < len; ++i) out[i] = (float)in[i] * (1.0f / 32768.0f);
}
