#include "ppa_h264x_access_unit.h"

#include "../common/mem64.h"

#include <limits.h>
#include <pspkernel.h>
#include <string.h>

#define PPA_H264X_FNV1A_OFFSET 2166136261U
#define PPA_H264X_FNV1A_PRIME  16777619U

static uint32_t ppa_h264x_au_align(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1U) & ~(alignment - 1U);
}

static int ppa_h264x_au_add_u32(uint32_t a, uint32_t b, uint32_t *out)
{
	if (out == 0 || UINT_MAX - a < b)
		return 0;
	*out = a + b;
	return 1;
}

static uint32_t ppa_h264x_au_next_capacity(uint32_t current,
                                           uint32_t required,
                                           uint32_t maximum)
{
	uint32_t value = current != 0U ? current : 8U;

	if (required > maximum)
		return 0U;
	while (value < required) {
		if (value > maximum / 2U) {
			value = maximum;
			break;
		}
		value *= 2U;
	}
	return value >= required ? value : 0U;
}

static int ppa_h264x_au_read_length(const uint8_t *source,
                                    uint32_t available,
                                    uint32_t length_size,
                                    uint32_t *length)
{
	uint32_t value = 0U;
	uint32_t i;

	if (source == 0 || length == 0 || length_size == 0U ||
	    length_size > 4U || available < length_size)
		return 0;
	for (i = 0U; i < length_size; ++i)
		value = (value << 8) | source[i];
	*length = value;
	return 1;
}

static int ppa_h264x_au_reserve_records(PpaH264xAccessUnitContext *context,
                                         uint32_t required)
{
	PpaH264xNalRecord *records;
	uint32_t capacity;
	uint64_t bytes;

	if (required <= context->record_capacity)
		return 1;
	capacity = ppa_h264x_au_next_capacity(context->record_capacity,
	                                      required,
	                                      PPA_H264X_AU_MAX_NALS);
	if (capacity == 0U)
		return 0;
	bytes = (uint64_t)capacity * sizeof(*records);
	if (bytes > (uint64_t)INT_MAX)
		return 0;
	records = (PpaH264xNalRecord *)malloc_64((int)bytes);
	if (records == 0)
		return 0;
	if (context->records != 0)
		free_64(context->records);
	context->records = records;
	context->record_capacity = capacity;
	return 1;
}

static int ppa_h264x_au_reserve_rbsp(PpaH264xAccessUnitContext *context,
                                      uint32_t required)
{
	uint8_t *rbsp;
	uint32_t capacity;

	if (required <= context->rbsp_capacity)
		return 1;
	capacity = context->rbsp_capacity != 0U ?
	           context->rbsp_capacity : 8192U;
	while (capacity < required) {
		if (capacity > (uint32_t)INT_MAX / 2U) {
			capacity = required;
			break;
		}
		capacity *= 2U;
	}
	if (capacity < required || capacity > (uint32_t)INT_MAX)
		return 0;
	rbsp = (uint8_t *)malloc_64((int)capacity);
	if (rbsp == 0)
		return 0;
	if (context->rbsp != 0)
		free_64(context->rbsp);
	context->rbsp = rbsp;
	context->rbsp_capacity = capacity;
	return 1;
}

static void ppa_h264x_au_clear_output(PpaH264xAccessUnitContext *context)
{
	context->source = 0;
	context->source_size = 0U;
	context->record_count = 0U;
	context->rbsp_bytes = 0U;
	context->feature_flags = 0U;
}

static int ppa_h264x_au_fallback(PpaH264xAccessUnitContext *context,
                                 int result,
                                 int lock_session)
{
	ppa_h264x_au_clear_output(context);
	context->last_result = result;
	if (lock_session)
		context->direct_fallback_locked = 1U;
	return result;
}

static uint32_t ppa_h264x_au_feature_for_type(uint32_t type)
{
	switch (type) {
	case 1U:
		return PPA_H264X_AU_HAS_VCL;
	case 5U:
		return PPA_H264X_AU_HAS_VCL | PPA_H264X_AU_HAS_IDR;
	case 6U:
		return PPA_H264X_AU_HAS_SEI;
	case 7U:
		return PPA_H264X_AU_HAS_SPS;
	case 8U:
		return PPA_H264X_AU_HAS_PPS;
	case 9U:
		return PPA_H264X_AU_HAS_AUD;
	case 2U:
	case 3U:
	case 4U:
		return PPA_H264X_AU_HAS_UNSUPPORTED_PARTITION;
	default:
		return 0U;
	}
}

static void ppa_h264x_au_emit_rbsp(const uint8_t *nal,
                                   uint32_t nal_size,
                                   uint8_t *output,
                                   uint32_t output_limit,
                                   uint32_t *stored_size,
                                   uint32_t *logical_size,
                                   uint32_t *checksum,
                                   uint32_t *scanned_bytes)
{
	uint32_t stored = 0U;
	uint32_t logical = 0U;
	uint32_t hash = PPA_H264X_FNV1A_OFFSET;
	uint32_t zero_count = 0U;
	uint32_t i;
	uint32_t scanned = 0U;

	for (i = 1U; i < nal_size; ++i) {
		uint8_t value = nal[i];
		++scanned;
		if (zero_count >= 2U && value == 0x03U) {
			/* The prevention byte terminates the escape sequence. Reset
			 * before examining the following RBSP byte so 00 00 03 03
			 * correctly produces 00 00 03 rather than dropping both 03s. */
			zero_count = 0U;
			continue;
		}
		if (stored < output_limit)
			output[stored++] = value;
		hash ^= value;
		hash *= PPA_H264X_FNV1A_PRIME;
		zero_count = value == 0U ?
		             (zero_count < 2U ? zero_count + 1U : 2U) : 0U;
		++logical;
		/* Stop after proving whether the stored prefix is truncated. */
		if (logical > output_limit)
			break;
	}
	*stored_size = stored;
	*logical_size = logical;
	*checksum = hash;
	*scanned_bytes = scanned;
}

static int ppa_h264x_au_should_verify(
    const PpaH264xAccessUnitContext *context)
{
	if (context->sequence <= PPA_H264X_AU_VERIFY_FIRST)
		return 1;
	return PPA_H264X_AU_VERIFY_INTERVAL != 0U &&
	       (context->sequence % PPA_H264X_AU_VERIFY_INTERVAL) == 0U;
}

static int ppa_h264x_au_verify(const PpaH264xAccessUnitContext *context,
                               uint32_t nal_length_size)
{
	const uint8_t *source = context->source;
	uint32_t source_size = context->source_size;
	uint32_t pos = 0U;
	uint32_t index = 0U;

	while (pos <= source_size &&
	       nal_length_size <= source_size - pos) {
		const PpaH264xNalRecord *record;
		const uint8_t *stored;
		uint32_t nal_size;
		uint32_t stored_index = 0U;
		uint32_t logical = 0U;
		uint32_t hash = PPA_H264X_FNV1A_OFFSET;
		uint32_t zero_count = 0U;
		uint32_t i;

		if (!ppa_h264x_au_read_length(source + pos,
		                             source_size - pos,
		                             nal_length_size, &nal_size))
			return 0;
		pos += nal_length_size;
		if (nal_size == 0U)
			continue;
		if (nal_size > source_size - pos ||
		    index >= context->record_count)
			return 0;
		record = &context->records[index];
		if (record->source_offset != pos ||
		    record->source_size != nal_size ||
		    record->nal_header != source[pos] ||
		    record->nal_unit_type != (source[pos] & 0x1fU) ||
		    record->nal_ref_idc != ((source[pos] >> 5) & 0x03U))
			return 0;
		stored = context->rbsp + record->rbsp_offset;
		for (i = 1U; i < nal_size; ++i) {
			uint8_t value = source[pos + i];
			if (zero_count >= 2U && value == 0x03U) {
				zero_count = 0U;
				continue;
			}
			if (stored_index < PPA_H264X_AU_RBSP_PREFIX_BYTES) {
				if (stored_index >= record->rbsp_size ||
				    stored[stored_index] != value)
					return 0;
				++stored_index;
			}
			/* Independent sampled audit still walks the COMPLETE NAL. Only
			 * its digest scope changes to match the explicit record contract. */
			if (!1 ||
			    logical <= PPA_H264X_AU_RBSP_PREFIX_BYTES) {
				hash ^= value;
				hash *= PPA_H264X_FNV1A_PRIME;
			}
			zero_count = value == 0U ?
			             (zero_count < 2U ? zero_count + 1U : 2U) : 0U;
			++logical;
		}
		if (logical > PPA_H264X_AU_RBSP_PREFIX_BYTES)
			logical = PPA_H264X_AU_RBSP_PREFIX_BYTES + 1U;
		if (record->flags !=
		    ((logical > stored_index ? PPA_H264X_NAL_RBSP_TRUNCATED : 0U) |
		     (1 ? PPA_H264X_NAL_PREFIX_DIGEST : 0U)))
			return 0;
		if (stored_index != record->rbsp_size ||
		    logical != record->rbsp_logical_size ||
		    hash != record->checksum)
			return 0;
		if (((logical > record->rbsp_size) ? 1U : 0U) !=
		    ((record->flags & PPA_H264X_NAL_RBSP_TRUNCATED) ? 1U : 0U))
			return 0;
		for (i = 0U; i < PPA_H264X_AU_RBSP_PADDING; ++i) {
			if (stored[record->rbsp_size + i] != 0U)
				return 0;
		}
		pos += nal_size;
		++index;
	}
	return pos == source_size && index == context->record_count;
}

void ppa_h264x_access_unit_init(PpaH264xAccessUnitContext *context)
{
	if (context == 0)
		return;
	memset(context, 0, sizeof(*context));
}

void ppa_h264x_access_unit_reset_stream(PpaH264xAccessUnitContext *context)
{
	if (context == 0)
		return;
	/* Buffer capacity and any session fallback lock survive seeks/resets. */
	ppa_h264x_au_clear_output(context);
	context->last_result = PPA_H264X_AU_OK;
}

void ppa_h264x_access_unit_close(PpaH264xAccessUnitContext *context)
{
	if (context == 0)
		return;
	if (context->records != 0)
		free_64(context->records);
	if (context->rbsp != 0)
		free_64(context->rbsp);
	memset(context, 0, sizeof(*context));
}

int ppa_h264x_access_unit_prepare(PpaH264xAccessUnitContext *context,
                                      const void *source_data,
                                      uint32_t source_size,
                                      uint32_t nal_length_size)
{
	const uint8_t *source = (const uint8_t *)source_data;
	uint32_t pos;
	uint32_t count;
	uint32_t required_rbsp;
	uint32_t write_offset;
	uint32_t index;

	if (context == 0)
		return PPA_H264X_AU_INVALID;
	ppa_h264x_au_clear_output(context);
	++context->sequence;

	if (context->direct_fallback_locked) {
		return ppa_h264x_au_fallback(context,
		                             PPA_H264X_AU_FALLBACK, 0);
	}
	if (source == 0 || source_size == 0U ||
	    nal_length_size == 0U || nal_length_size > 4U) {
		return ppa_h264x_au_fallback(context,
		                             PPA_H264X_AU_INVALID, 1);
	}

	pos = 0U;
	count = 0U;
	required_rbsp = 0U;
	while (pos <= source_size &&
	       nal_length_size <= source_size - pos) {
		uint32_t nal_size;
		uint32_t stored_worst;
		uint32_t aligned;
		uint32_t next;

		if (!ppa_h264x_au_read_length(source + pos,
		                             source_size - pos,
		                             nal_length_size, &nal_size)) {
			return ppa_h264x_au_fallback(context,
			                             PPA_H264X_AU_INVALID, 1);
		}
		pos += nal_length_size;
		if (nal_size == 0U)
			continue;
		if (nal_size > source_size - pos ||
		    count >= PPA_H264X_AU_MAX_NALS) {
			return ppa_h264x_au_fallback(context,
			                             PPA_H264X_AU_INVALID, 1);
		}
		aligned = ppa_h264x_au_align(required_rbsp,
		                            PPA_H264X_AU_RBSP_ALIGNMENT);
		stored_worst = nal_size > 1U ? nal_size - 1U : 0U;
		if (stored_worst > PPA_H264X_AU_RBSP_PREFIX_BYTES)
			stored_worst = PPA_H264X_AU_RBSP_PREFIX_BYTES;
		if (!ppa_h264x_au_add_u32(aligned, stored_worst, &next) ||
		    !ppa_h264x_au_add_u32(next,
		                          PPA_H264X_AU_RBSP_PADDING,
		                          &required_rbsp)) {
			return ppa_h264x_au_fallback(context,
			                             PPA_H264X_AU_INVALID, 1);
		}
		++count;
		pos += nal_size;
	}
	if (pos != source_size || count == 0U) {
		return ppa_h264x_au_fallback(context,
		                             PPA_H264X_AU_INVALID, 1);
	}
	if (!ppa_h264x_au_reserve_records(context, count) ||
	    !ppa_h264x_au_reserve_rbsp(context, required_rbsp)) {
		return ppa_h264x_au_fallback(context,
		                             PPA_H264X_AU_NO_MEMORY, 1);
	}

	pos = 0U;
	index = 0U;
	write_offset = 0U;
	context->source = source;
	context->source_size = source_size;
	context->feature_flags = 0U;
	context->rbsp_bytes = 0U;
	while (pos <= source_size &&
	       nal_length_size <= source_size - pos && index < count) {
		PpaH264xNalRecord *record;
		uint32_t nal_size;
		uint32_t stored_size;
		uint32_t logical_size;
		uint32_t checksum;
		uint32_t scanned;

		(void)ppa_h264x_au_read_length(source + pos,
		                              source_size - pos,
		                              nal_length_size, &nal_size);
		pos += nal_length_size;
		if (nal_size == 0U)
			continue;
		record = &context->records[index];
		memset(record, 0, sizeof(*record));
		write_offset = ppa_h264x_au_align(
		    write_offset, PPA_H264X_AU_RBSP_ALIGNMENT);
		record->source_offset = pos;
		record->source_size = nal_size;
		record->rbsp_offset = write_offset;
		record->nal_header = source[pos];
		record->nal_unit_type = source[pos] & 0x1fU;
		record->nal_ref_idc = (source[pos] >> 5) & 0x03U;
		ppa_h264x_au_emit_rbsp(source + pos, nal_size,
		                       context->rbsp + write_offset,
		                       PPA_H264X_AU_RBSP_PREFIX_BYTES,
		                       &stored_size, &logical_size, &checksum, &scanned);
		record->rbsp_size = stored_size;
		record->rbsp_logical_size = logical_size;
		record->checksum = checksum;
		record->flags |= PPA_H264X_NAL_PREFIX_DIGEST;
		if (logical_size > stored_size)
			record->flags |= PPA_H264X_NAL_RBSP_TRUNCATED;
		memset(context->rbsp + write_offset + stored_size, 0,
		       PPA_H264X_AU_RBSP_PADDING);
		context->feature_flags |=
		    ppa_h264x_au_feature_for_type(record->nal_unit_type);
		context->rbsp_bytes += stored_size;
		write_offset += stored_size + PPA_H264X_AU_RBSP_PADDING;
		pos += nal_size;
		++index;
	}
	context->record_count = index;

	if (ppa_h264x_au_should_verify(context)) {
		if (!ppa_h264x_au_verify(context, nal_length_size)) {
			return ppa_h264x_au_fallback(
			    context, PPA_H264X_AU_VERIFY_MISMATCH, 1);
		}
	}

	context->last_result = PPA_H264X_AU_OK;
	return PPA_H264X_AU_OK;
}

const PpaH264xNalRecord *ppa_h264x_access_unit_record(
    const PpaH264xAccessUnitContext *context,
    uint32_t index)
{
	if (context == 0 || index >= context->record_count)
		return 0;
	return &context->records[index];
}

const uint8_t *ppa_h264x_access_unit_rbsp(
    const PpaH264xAccessUnitContext *context,
    const PpaH264xNalRecord *record)
{
	if (context == 0 || record == 0 || context->rbsp == 0 ||
	    record->rbsp_offset > context->rbsp_capacity ||
	    record->rbsp_size > context->rbsp_capacity - record->rbsp_offset)
		return 0;
	return context->rbsp + record->rbsp_offset;
}
