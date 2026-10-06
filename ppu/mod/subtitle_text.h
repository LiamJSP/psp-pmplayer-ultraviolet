#ifndef subtitle_text_h__
#define subtitle_text_h__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SUBTITLE_TEXT_TYPE_UTF8   0x74787475U /* txtu */
#define SUBTITLE_TEXT_TYPE_ASCII  0x7478746CU /* txtl */
#define SUBTITLE_TEXT_TYPE_SSA    0x73736175U /* ssau */
#define SUBTITLE_TEXT_TYPE_ASS    0x61737375U /* assu */
#define SUBTITLE_TEXT_TYPE_WEBVTT 0x76747475U /* vttu */

#define SUBTITLE_TEXT_FLAG_NONE              0x00000000U
#define SUBTITLE_TEXT_FLAG_COMPLEX_UNSHAPED  0x00000001U
#define SUBTITLE_TEXT_FLAG_RTL               0x00000002U
#define SUBTITLE_TEXT_FLAG_ARABIC_SHAPED     0x00000004U
#define SUBTITLE_TEXT_FLAG_CJK               0x00000008U

int subtitle_text_normalize_mkv_payload(char *dst,
                                        size_t dst_size,
                                        unsigned int *out_lines,
                                        unsigned int *out_flags,
                                        const uint8_t *payload,
                                        uint64_t payload_size,
                                        uint32_t subtitle_type,
                                        const uint8_t *codec_private,
                                        uint32_t codec_private_size);

#ifdef __cplusplus
}
#endif

#endif