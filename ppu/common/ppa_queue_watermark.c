#include "ppa_queue_watermark.h"

#include "buffered_reader.h"

#include <limits.h>
#include <string.h>

static uint32_t ppa_queue_u64_to_u32(uint64_t value)
{
	return value > UINT_MAX ? UINT_MAX : (uint32_t)value;
}

static uint32_t ppa_queue_nominal_duration(
                                  const struct PpaQueueWatermark *watermark,
                                  uint32_t items)
{
	uint64_t duration;

	if (watermark == 0 || items == 0U)
		return 0U;
	duration = (uint64_t)items *
	           (uint64_t)(watermark->nominal_item_ms != 0U ?
	                      watermark->nominal_item_ms : 1U);
	return ppa_queue_u64_to_u32(duration);
}

void ppa_queue_watermark_init(struct PpaQueueWatermark *watermark,
                              uint32_t low_ms,
                              uint32_t target_ms,
                              uint32_t byte_ceiling,
                              uint32_t minimum_items,
                              uint32_t nominal_item_ms)
{
	if (watermark == 0)
		return;
	memset(watermark, 0, sizeof(*watermark));
	if (target_ms < low_ms)
		target_ms = low_ms;
	watermark->low_ms = low_ms;
	watermark->target_ms = target_ms;
	watermark->byte_ceiling = byte_ceiling;
	watermark->minimum_items = minimum_items != 0U ? minimum_items : 1U;
	watermark->nominal_item_ms = nominal_item_ms != 0U ?
	                             nominal_item_ms : 1U;
	/* Startup is not an underrun edge. The latch is released only after the
	 * queue has first established healthy margin. */
	watermark->low_latched = 1U;
}

void ppa_queue_watermark_set_minimum_items(
                              struct PpaQueueWatermark *watermark,
                              uint32_t minimum_items,
                              uint32_t queue_capacity)
{
	if (watermark == 0)
		return;
	if (minimum_items == 0U)
		minimum_items = 1U;
	if (queue_capacity > 1U && minimum_items >= queue_capacity)
		minimum_items = queue_capacity - 1U;
	watermark->minimum_items = minimum_items;
}

void ppa_queue_watermark_refresh(struct PpaQueueWatermark *watermark,
                                 const struct ppa_media_packet *storage,
                                 uint32_t front,
                                 uint32_t size,
                                 uint32_t capacity)
{
	uint64_t bytes = 0U;
	uint32_t largest = 0U;
	uint32_t i;
	int have_timestamp = 0;
	int minimum_timestamp = 0;
	int maximum_timestamp = 0;
	uint32_t timestamp_span = 0U;
	uint32_t nominal_span;
	int low_now;

	if (watermark == 0)
		return;
	if (storage == 0 || capacity == 0U)
		size = 0U;
	if (size > capacity)
		size = capacity;

	for (i = 0U; i < size; ++i) {
		const struct ppa_media_packet *packet =
			&storage[(front + i) % capacity];
		bytes += packet->size;
		if (packet->size > largest)
			largest = packet->size;
		if (packet->timestamp >= 0) {
			if (!have_timestamp) {
				minimum_timestamp = packet->timestamp;
				maximum_timestamp = packet->timestamp;
				have_timestamp = 1;
			}
			else {
				if (packet->timestamp < minimum_timestamp)
					minimum_timestamp = packet->timestamp;
				if (packet->timestamp > maximum_timestamp)
					maximum_timestamp = packet->timestamp;
			}
		}
	}

	nominal_span = ppa_queue_nominal_duration(watermark, size);
	if (have_timestamp && maximum_timestamp >= minimum_timestamp) {
		uint64_t span = (uint64_t)((int64_t)maximum_timestamp -
		                           (int64_t)minimum_timestamp) +
		                watermark->nominal_item_ms;
		timestamp_span = ppa_queue_u64_to_u32(span);
	}

	watermark->queued_items = size;
	watermark->queued_bytes = ppa_queue_u64_to_u32(bytes);
	watermark->queued_duration_ms = timestamp_span > nominal_span ?
	                                timestamp_span : nominal_span;
	watermark->largest_packet_bytes = largest;

	low_now = size < watermark->minimum_items ||
	          watermark->queued_duration_ms <= watermark->low_ms;
	if (!low_now) {
		watermark->healthy_seen = 1U;
		watermark->low_latched = 0U;
	}
	else if (watermark->healthy_seen && !watermark->low_latched) {
		watermark->low_latched = 1U;
		watermark->low_events++;
	}
}

void ppa_queue_watermark_reset(struct PpaQueueWatermark *watermark)
{
	uint32_t low_ms;
	uint32_t target_ms;
	uint32_t byte_ceiling;
	uint32_t minimum_items;
	uint32_t nominal_item_ms;
	uint64_t low_events;
	uint64_t target_stops;
	uint64_t byte_stops;

	if (watermark == 0)
		return;
	low_ms = watermark->low_ms;
	target_ms = watermark->target_ms;
	byte_ceiling = watermark->byte_ceiling;
	minimum_items = watermark->minimum_items;
	nominal_item_ms = watermark->nominal_item_ms;
	low_events = watermark->low_events;
	target_stops = watermark->target_stops;
	byte_stops = watermark->byte_stops;
	ppa_queue_watermark_init(watermark, low_ms, target_ms, byte_ceiling,
	                         minimum_items, nominal_item_ms);
	watermark->low_events = low_events;
	watermark->target_stops = target_stops;
	watermark->byte_stops = byte_stops;
}

int ppa_queue_watermark_is_low(const struct PpaQueueWatermark *watermark)
{
	if (watermark == 0)
		return 1;
	return watermark->queued_items < watermark->minimum_items ||
	       watermark->queued_duration_ms <= watermark->low_ms;
}

int ppa_queue_watermark_target_reached(struct PpaQueueWatermark *watermark)
{
	int byte_reached;
	int time_reached;

	if (watermark == 0)
		return 0;
	byte_reached = watermark->byte_ceiling != 0U &&
	               watermark->queued_bytes >= watermark->byte_ceiling;
	time_reached = watermark->queued_items >= watermark->minimum_items &&
	               watermark->queued_duration_ms >= watermark->target_ms;

	if (byte_reached) {
		if (!watermark->byte_latched) {
			watermark->byte_latched = 1U;
			watermark->byte_stops++;
		}
	}
	else {
		watermark->byte_latched = 0U;
	}
	if (time_reached) {
		if (!watermark->target_latched) {
			watermark->target_latched = 1U;
			watermark->target_stops++;
		}
	}
	else {
		watermark->target_latched = 0U;
	}
	return byte_reached || time_reached;
}

uint32_t ppa_queue_watermark_margin_ms(
                                 const struct PpaQueueWatermark *watermark)
{
	if (watermark == 0 ||
	    watermark->queued_duration_ms <= watermark->low_ms)
		return 0U;
	return watermark->queued_duration_ms - watermark->low_ms;
}

void ppa_queue_watermark_schedule_reader(
                                 const struct PpaQueueWatermark *watermark,
                                 buffered_reader_t *reader)
{
	if (watermark == 0 || reader == 0)
		return;
	buffered_reader_set_queue_watermarks(reader,
	                                     watermark->queued_duration_ms,
	                                     watermark->low_ms,
	                                     watermark->target_ms,
	                                     watermark->queued_bytes,
	                                     watermark->byte_ceiling);
}
