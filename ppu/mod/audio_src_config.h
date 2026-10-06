/* Private configuration for the unmodified libsamplerate 0.2.2 sources.
 * Do not expose these generic package macros to other application units. */
#define HAVE_STDBOOL_H 1
#if PPU_AUDIO_SINC_DEFAULT
#define ENABLE_SINC_FAST_CONVERTER 1
#endif
#define CPU_CLIPS_POSITIVE 0
#define CPU_CLIPS_NEGATIVE 0
#define PACKAGE "libsamplerate"
#define VERSION "0.2.2"
