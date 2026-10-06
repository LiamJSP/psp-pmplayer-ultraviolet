#ifndef PPA_QUEUE_WATERMARK_H
#define PPA_QUEUE_WATERMARK_H

#include <stdint.h>

#include "ppa_media_packet.h"
#include "buffered_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime queue policy. The fixed packet arrays remain unchanged; this state
 * limits how much of them steady-state demux is allowed to occupy. */
struct PpaQueueWatermark {
	uint32_t low_ms;
	uint32_t target_ms;
	uint32_t byte_ceiling;
	uint32_t minimum_items;
	uint32_t nominal_item_ms;

	uint32_t queued_items;
	uint32_t queued_bytes;
	/* An estimate from packet PTS range or nominal cadence, whichever is
	 * larger. Sparse timestamp gaps can overstate playable coverage. */
	uint32_t queued_duration_ms;
	uint32_t largest_packet_bytes;

	uint32_t low_latched;
	uint32_t target_latched;
	uint32_t byte_latched;
	uint32_t healthy_seen;
	uint64_t low_events;   /* crossings below the low threshold, not underruns */
	uint64_t target_stops; /* crossings into time-based refill backpressure */
	uint64_t byte_stops;   /* crossings into byte-based refill backpressure */
};

void ppa_queue_watermark_init(struct PpaQueueWatermark *watermark,
                              uint32_t low_ms,
                              uint32_t target_ms,
                              uint32_t byte_ceiling,
                              uint32_t minimum_items,
                              uint32_t nominal_item_ms);

void ppa_queue_watermark_set_minimum_items(
                              struct PpaQueueWatermark *watermark,
                              uint32_t minimum_items,
                              uint32_t queue_capacity);

/* Rebuilds byte/time accounting from the existing ring. This deliberately
 * tolerates non-monotonic B-frame PTS by measuring the full timestamp span. */
void ppa_queue_watermark_refresh(struct PpaQueueWatermark *watermark,
                                 const struct ppa_media_packet *storage,
                                 uint32_t front,
                                 uint32_t size,
                                 uint32_t capacity);

void ppa_queue_watermark_reset(struct PpaQueueWatermark *watermark);

int ppa_queue_watermark_is_low(const struct PpaQueueWatermark *watermark);
int ppa_queue_watermark_target_reached(struct PpaQueueWatermark *watermark);
uint32_t ppa_queue_watermark_margin_ms(
                                 const struct PpaQueueWatermark *watermark);

/* Publishes this queue's deadline/not-before policy to a logical reader. */
void ppa_queue_watermark_schedule_reader(
                                 const struct PpaQueueWatermark *watermark,
                                 buffered_reader_t *reader);

#ifdef __cplusplus
}
#endif

#endif
