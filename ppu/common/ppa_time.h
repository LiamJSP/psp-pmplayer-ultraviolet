#ifndef PPA_TIME_H
#define PPA_TIME_H

#include <stdint.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint64_t ppa_div_round_u64(uint64_t num, uint64_t den)
{
	if (den == 0)
		return 0;

	if (num > UINT64_MAX - den / 2)
		return num / den;

	return (num + den / 2) / den;
}

static inline unsigned ppa_timestamp_ms_to_frame(unsigned timestamp_ms,
                                                 unsigned video_rate,
                                                 unsigned video_scale)
{
	uint64_t den;

	if (video_rate == 0 || video_scale == 0)
		return 0;

	den = (uint64_t)video_scale * 1000ULL;

	return (unsigned)ppa_div_round_u64((uint64_t)timestamp_ms *
	                                   (uint64_t)video_rate,
	                                   den);
}

static inline int64_t ppa_mkv_timecode_to_ms(int64_t timecode_delta,
                                             uint64_t timecode_scale)
{
	int sign = 1;
	uint64_t value;
	uint64_t scaled;

	if (timecode_scale == 0)
		timecode_scale = 1000000ULL;

	if (timecode_delta < 0) {
		sign = -1;

		if (timecode_delta == INT64_MIN)
			value = ((uint64_t)INT64_MAX) + 1ULL;
		else
			value = (uint64_t)(-timecode_delta);
	}
	else {
		value = (uint64_t)timecode_delta;
	}

	if (value != 0 && timecode_scale > UINT64_MAX / value)
		return sign > 0 ? INT64_MAX : INT64_MIN;

	scaled = ppa_div_round_u64(value * timecode_scale, 1000000ULL);

	if (scaled > (uint64_t)INT64_MAX)
		return sign > 0 ? INT64_MAX : INT64_MIN;

	return sign > 0 ? (int64_t)scaled : -(int64_t)scaled;
}

#ifdef __cplusplus
}
#endif

#endif