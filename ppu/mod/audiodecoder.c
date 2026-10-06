#include "audiodecoder.h"
#include "common/mem64.h"

#include <pspaudiocodec.h>
#include <stdint.h>
#include <string.h>

static unsigned long g_codec[65] __attribute__((aligned(64)));
static short g_pcm[1152 * 2] __attribute__((aligned(64)));
static int g_open;
static int g_type;
/* Firmware may update control words. Keep allocation ownership separately. */
static void *g_codec_memory;

static int codec_allocate_workspace(unsigned long *codec)
{
    void *memory;
    if (codec == 0 || codec[4] == 0)
        return -1;
    memory = malloc_64((unsigned int)codec[4]);
    if (memory == 0)
        return -1;
    g_codec_memory = memory;
    codec[3] = (unsigned long)(uintptr_t)memory;
    return 0;
}

static void codec_release_workspace(unsigned long *codec)
{
    if (g_codec_memory != 0) {
        free_64(g_codec_memory);
        g_codec_memory = 0;
    }
    if (codec != 0) codec[3] = 0;
}

int audio_decoder_open(int type, int samplerate, int samplecount, int blockalign)
{
    (void)samplecount;
    (void)blockalign;

    audio_decoder_close();
    if (type != PSP_CODEC_AUDIO_AAC && type != PSP_CODEC_AUDIO_MP3)
        return -1;
    memset(g_codec, 0, sizeof(g_codec));

    if (sceAudiocodecCheckNeedMem(g_codec, type) < 0 ||
        codec_allocate_workspace(g_codec) < 0) {
        audio_decoder_close();
        return -1;
    }

    /* AAC requires its output sample rate in word 10.  MP3 overwrites word 10
     * with each compressed frame size immediately before decode. */
    if (type == PSP_CODEC_AUDIO_AAC)
        g_codec[10] = (unsigned long)samplerate;

    if (sceAudiocodecInit(g_codec, type) < 0) {
        audio_decoder_close();
        return -1;
    }

    g_type = type;
    g_open = 1;
    return 0;
}

int audio_decoder_close(void)
{
    /* This backend supplies main-RAM codec workspace, never GetEDRAM. Do not
     * call ReleaseEDRAM on an allocation owned by malloc_64. The synchronous
     * Decode owner must have returned/joined before this release. */
    codec_release_workspace(g_codec);
    memset(g_codec, 0, sizeof(g_codec));
    memset(g_pcm, 0, sizeof(g_pcm));
    g_type = 0;
    g_open = 0;
    return 0;
}

int audio_decoder_is_open(void) { return g_open || g_codec_memory != 0; }

int audio_decoder_decode(void *data,
                         int *data_size,
                         uint8_t *buffer,
                         int buffer_size, unsigned int pcm_frames)
{
    unsigned int output_bytes;
    int decode_result;

    if (data_size != 0) *data_size = 0;
    if (!g_open || data == 0 || data_size == 0 || buffer == 0 ||
        buffer_size <= 0)
        return -1;

    output_bytes = (g_type == PSP_CODEC_AUDIO_MP3) ? 0x1200U : 0x1000U;
    if (!pcm_frames || pcm_frames > output_bytes / 4U) return -1;
    g_codec[6] = (unsigned long)(uintptr_t)buffer;
    g_codec[8] = (unsigned long)(uintptr_t)g_pcm;
    g_codec[7] = (unsigned long)buffer_size;
    g_codec[9] = output_bytes;
    if (g_type == PSP_CODEC_AUDIO_MP3)
        g_codec[10] = (unsigned long)buffer_size;

    decode_result = sceAudiocodecDecode(g_codec, g_type);
    if (decode_result < 0) {
        memset(g_pcm, 0, output_bytes);
        /* Let MP4/MKV stop and close the firmware session. Reporting success
         * here used to keep submitting packets after native codec failures. */
        return -1;
    }

    /* MPEG-2 Layer III uses half the MPEG-1 workspace. Do not copy its unused
     * tail into staging; retain g_pcm's firmware alignment and lifetime. */
    memcpy(data, g_pcm, pcm_frames * 4U);
    *data_size = (int)(pcm_frames * 4U);
    return buffer_size;
}
