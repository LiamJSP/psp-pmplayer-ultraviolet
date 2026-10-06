#ifndef PPA_H264X_ACCESS_UNIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PPA_H264X_AU_MAX_NALS
#define PPA_H264X_AU_MAX_NALS 1024U
#endif

#ifndef PPA_H264X_AU_RBSP_PREFIX_BYTES
#define PPA_H264X_AU_RBSP_PREFIX_BYTES 4096U
#endif

#ifndef PPA_H264X_AU_VERIFY_FIRST
#define PPA_H264X_AU_VERIFY_FIRST 8U
#endif

#ifndef PPA_H264X_AU_VERIFY_INTERVAL
#define PPA_H264X_AU_VERIFY_INTERVAL 128U
#endif

#define PPA_H264X_AU_RBSP_ALIGNMENT 16U
#define PPA_H264X_AU_RBSP_PADDING   16U

enum PpaH264xAccessUnitResult {
	PPA_H264X_AU_OK = 0,
	PPA_H264X_AU_FALLBACK = -1,
	PPA_H264X_AU_INVALID = -2,
	PPA_H264X_AU_NO_MEMORY = -3,
	PPA_H264X_AU_VERIFY_MISMATCH = -4
};

enum PpaH264xAccessUnitFeature {
	PPA_H264X_AU_HAS_VCL = 1U << 0,
	PPA_H264X_AU_HAS_IDR = 1U << 1,
	PPA_H264X_AU_HAS_SPS = 1U << 2,
	PPA_H264X_AU_HAS_PPS = 1U << 3,
	PPA_H264X_AU_HAS_SEI = 1U << 4,
	PPA_H264X_AU_HAS_AUD = 1U << 5,
	PPA_H264X_AU_HAS_UNSUPPORTED_PARTITION = 1U << 6
};

enum PpaH264xNalRecordFlag {
	PPA_H264X_NAL_RBSP_TRUNCATED = 1U << 0,
	PPA_H264X_NAL_PREFIX_DIGEST = 1U << 1
};

/*
 * source_offset points at the NAL header, not its AVCC length word.
 * rbsp_size is the stored, padded semantic prefix; rbsp_logical_size is the
 * complete payload size after emulation-prevention removal. The header byte is
 * intentionally excluded from both RBSP sizes. With PREFIX_DIGEST, logical
 * size is min(full size, prefix limit + 1), and checksum covers those examined
 * RBSP bytes only. The one-byte lookahead proves truncation, including escaped
 * endings. Neither field then describes or validates the unexamined tail.
 * Semantic consumers must use rbsp_size; Sony receives the original full NAL.
 */
typedef struct PpaH264xNalRecord {
	uint32_t source_offset;
	uint32_t source_size;
	uint32_t rbsp_offset;
	uint32_t rbsp_size;
	uint32_t rbsp_logical_size;
	uint32_t checksum;
	uint16_t flags;
	uint8_t nal_header;
	uint8_t nal_unit_type;
	uint8_t nal_ref_idc;
	uint8_t reserved[3];
} PpaH264xNalRecord;

typedef struct PpaH264xAccessUnitContext {
	PpaH264xNalRecord *records;
	uint8_t *rbsp;
	uint32_t record_capacity;
	uint32_t rbsp_capacity;

	const uint8_t *source;
	uint32_t source_size;
	uint32_t record_count;
	uint32_t rbsp_bytes;
	uint32_t feature_flags;
	uint32_t sequence;
	uint32_t direct_fallback_locked;
	int last_result;
} PpaH264xAccessUnitContext;

void ppa_h264x_access_unit_init(PpaH264xAccessUnitContext *context);
void ppa_h264x_access_unit_reset_stream(PpaH264xAccessUnitContext *context);
void ppa_h264x_access_unit_close(PpaH264xAccessUnitContext *context);

int ppa_h264x_access_unit_prepare(PpaH264xAccessUnitContext *context,
                                  const void *source, uint32_t source_size,
                                  uint32_t nal_length_size);

const PpaH264xNalRecord *ppa_h264x_access_unit_record(
    const PpaH264xAccessUnitContext *context,
    uint32_t index);
const uint8_t *ppa_h264x_access_unit_rbsp(
    const PpaH264xAccessUnitContext *context,
    const PpaH264xNalRecord *record);

#ifdef __cplusplus
}
#endif

#endif
