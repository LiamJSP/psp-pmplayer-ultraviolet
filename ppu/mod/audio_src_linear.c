#include "audio_src_config.h"
#ifndef PPU_AUDIO_FAST_LINEAR
#define PPU_AUDIO_FAST_LINEAR 1
#endif
#if PPU_AUDIO_FAST_LINEAR
/* Only the constructor is renamed: upstream reset/copy/close and its generic
 * variable-rate converter remain available in this private translation unit. */
#define linear_state_new ppu_linear_state_new_upstream
#endif
#include "../../third_party/libsamplerate-0.2.2/src/src_linear.c"
#if PPU_AUDIO_FAST_LINEAR
#undef linear_state_new
#include "audio_src_linear_psp.h"
#endif
