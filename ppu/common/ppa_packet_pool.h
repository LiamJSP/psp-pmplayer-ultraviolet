#ifndef PPA_PACKET_POOL_H
#define PPA_PACKET_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

#define PPA_PACKET_POOL_HISTOGRAM_BUCKETS 12U

enum ppa_packet_pool_state {
	PPA_PACKET_FREE = 0,
	PPA_PACKET_FILLING,
	PPA_PACKET_QUEUED,
	PPA_PACKET_DECODING,
	PPA_PACKET_READY_TO_REUSE
};

enum ppa_packet_pool_kind {
	PPA_PACKET_POOL_KIND_UNKNOWN = 0,
	PPA_PACKET_POOL_KIND_VIDEO,
	PPA_PACKET_POOL_KIND_AUDIO
};

struct ppa_packet_slot {
	void *data;
	unsigned capacity;
	unsigned size;
	unsigned timestamp;
	unsigned state;
	unsigned heap_fallback;
	unsigned me_readers;
	unsigned me_quarantined;
};

struct ppa_packet_pool {
	struct ppa_packet_slot *slots;
	unsigned slot_count;
	unsigned slot_capacity;

	unsigned kind;
	unsigned active_slots;
	unsigned allocated_bytes;

	/* Acquisition-size history is reset only at explicit safe trim points. It
	 * gives seek/track/reset trimming a recent p95 rather than reacting to one
	 * exceptional packet or shrinking on every release. */
	unsigned size_histogram[PPA_PACKET_POOL_HISTOGRAM_BUCKETS];
	unsigned histogram_samples;
	struct ppa_packet_pool *me_registry_next;
};

/* Only owned, whole-line packet allocations qualify for zero-copy ME reads.
 * Borrowed I/O windows intentionally remain SC until they have a lease ABI. */
struct ppa_packet_slot *ppa_packet_pool_me_pin(const void *source, unsigned bytes);
void ppa_packet_pool_me_unpin(struct ppa_packet_slot *slot, int quarantine);
int ppa_packet_pool_me_has_quarantine(void);

struct ppa_packet_pool_trim_result {
	unsigned slots_freed;
	unsigned bytes_freed;
	unsigned retain_capacity;
};

int ppa_packet_pool_init(struct ppa_packet_pool *pool,
                         unsigned slot_count,
                         unsigned slot_capacity);

void ppa_packet_pool_set_kind(struct ppa_packet_pool *pool,
                                    unsigned kind);

void ppa_packet_pool_close(struct ppa_packet_pool *pool);

struct ppa_packet_slot *ppa_packet_pool_acquire(struct ppa_packet_pool *pool,
                                                unsigned required_size);

void ppa_packet_pool_release(struct ppa_packet_pool *pool,
                             struct ppa_packet_slot *slot);

/* Safe-event trim: only FREE slots far above the recent p95 are released.
 * floor_capacity protects the common packet size for the stream. */
void ppa_packet_pool_trim_free(struct ppa_packet_pool *pool,
                               unsigned floor_capacity,
                               struct ppa_packet_pool_trim_result *out);

#ifdef __cplusplus
}
#endif

#endif
