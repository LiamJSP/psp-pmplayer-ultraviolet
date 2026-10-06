#include "ppa_packet_pool.h"
#include "mem64.h"

#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>

static struct ppa_packet_pool *g_me_pools;
static unsigned g_me_quarantined;

struct ppa_packet_slot *ppa_packet_pool_me_pin(const void *source, unsigned bytes)
{
	struct ppa_packet_pool *p;
	struct ppa_packet_slot *found = 0;
	unsigned i;
	int irq;
	if (!source || !bytes || ((uintptr_t)source & 63U) || bytes > 0xffffffc0U)
		return 0;
	irq = sceKernelCpuSuspendIntr();
	for (p = g_me_pools; p && !found; p = p->me_registry_next) {
		for (i = 0; i < p->slot_count; ++i) {
			struct ppa_packet_slot *s = &p->slots[i];
			if (s->data == source && s->capacity >= ((bytes + 63U) & ~63U) &&
			    s->state != PPA_PACKET_FREE && s->state != PPA_PACKET_FILLING &&
			    !s->me_quarantined && !s->me_readers) {
				s->me_readers = 1U;
				found = s;
				break;
			}
		}
	}
	sceKernelCpuResumeIntr(irq);
	return found;
}

void ppa_packet_pool_me_unpin(struct ppa_packet_slot *slot, int quarantine)
{
	int irq;
	if (!slot) return;
	irq = sceKernelCpuSuspendIntr();
	if (quarantine) {
		slot->me_quarantined = 1U;
		g_me_quarantined = 1U;
	}
	slot->me_readers = 0U;
	sceKernelCpuResumeIntr(irq);
}

int ppa_packet_pool_me_has_quarantine(void) { return g_me_quarantined != 0U; }

static unsigned round64(unsigned v) {
	if (v == 0)
		v = 1;
	if (v > 0xffffffc0U)
		return 0xffffffc0U;

	return (v + 63U) & ~63U;
}

static const unsigned packet_size_bucket_limit[
    PPA_PACKET_POOL_HISTOGRAM_BUCKETS] = {
	4U * 1024U,
	8U * 1024U,
	16U * 1024U,
	32U * 1024U,
	64U * 1024U,
	128U * 1024U,
	256U * 1024U,
	512U * 1024U,
	1024U * 1024U,
	2U * 1024U * 1024U,
	4U * 1024U * 1024U,
	0xffffffc0U
};

static void note_packet_size(struct ppa_packet_pool *pool,
                             unsigned required_size)
{
	unsigned i;

	if (pool == 0)
		return;
	for (i = 0U; i + 1U < PPA_PACKET_POOL_HISTOGRAM_BUCKETS; ++i) {
		if (required_size <= packet_size_bucket_limit[i])
			break;
	}
	if (pool->size_histogram[i] != 0xffffffffU)
		pool->size_histogram[i]++;
	if (pool->histogram_samples != 0xffffffffU)
		pool->histogram_samples++;
}

static unsigned packet_pool_recent_p95(const struct ppa_packet_pool *pool)
{
	uint64_t target64;
	unsigned target;
	unsigned cumulative = 0U;
	unsigned i;

	if (pool == 0 || pool->histogram_samples == 0U)
		return pool != 0 ? pool->slot_capacity : 0U;
	target64 = ((uint64_t)pool->histogram_samples * 95ULL + 99ULL) / 100ULL;
	target = target64 > 0xffffffffULL ? 0xffffffffU : (unsigned)target64;
	if (target == 0U)
		target = 1U;
	for (i = 0U; i < PPA_PACKET_POOL_HISTOGRAM_BUCKETS; ++i) {
		if (0xffffffffU - cumulative < pool->size_histogram[i])
			cumulative = 0xffffffffU;
		else
			cumulative += pool->size_histogram[i];
		if (cumulative >= target)
			return packet_size_bucket_limit[i];
	}
	return packet_size_bucket_limit[PPA_PACKET_POOL_HISTOGRAM_BUCKETS - 1U];
}

static void note_acquire_success(struct ppa_packet_pool *pool,
                                 unsigned required_size) {

	if (pool == 0)
		return;

	note_packet_size(pool, required_size);

	pool->active_slots++;

}

static void note_acquire_fail(struct ppa_packet_pool *pool) {

	if (pool == 0)
		return;

}

static void note_grow(struct ppa_packet_pool *pool,
                      unsigned old_capacity,
                      unsigned new_capacity) {

	if (pool == 0)
		return;

	if (new_capacity >= old_capacity)
		pool->allocated_bytes += new_capacity - old_capacity;
	else
		pool->allocated_bytes -= old_capacity - new_capacity;

}

int ppa_packet_pool_init(struct ppa_packet_pool *pool,
                         unsigned slot_count,
                         unsigned slot_capacity) {
	if (!pool || slot_count == 0 || slot_capacity == 0)
		return 0;

	memset(pool, 0, sizeof(*pool));

	pool->slots = (struct ppa_packet_slot *)calloc(slot_count,
	                                               sizeof(struct ppa_packet_slot));
	if (!pool->slots)
		return 0;

	pool->slot_count = slot_count;
	pool->slot_capacity = round64(slot_capacity);
	pool->kind = PPA_PACKET_POOL_KIND_UNKNOWN;
	pool->active_slots = 0;
	pool->allocated_bytes = 0;
	{
		int irq = sceKernelCpuSuspendIntr();
		pool->me_registry_next = g_me_pools;
		g_me_pools = pool;
		sceKernelCpuResumeIntr(irq);
	}

	return 1;
}

void ppa_packet_pool_set_kind(struct ppa_packet_pool *pool,
                                    unsigned kind) {
	if (pool == 0)
		return;

	pool->kind = kind;
}

void ppa_packet_pool_close(struct ppa_packet_pool *pool) {
	unsigned i;

	if (!pool)
		return;
	/* Detach first: the containing reader may be freed even when an ME lease
	 * forces us to retain the separately allocated slot/payload storage. */
	{
		struct ppa_packet_pool **link;
		int irq = sceKernelCpuSuspendIntr();
		for (link = &g_me_pools; *link; link = &(*link)->me_registry_next) {
			if (*link == pool) { *link = pool->me_registry_next; break; }
		}
		sceKernelCpuResumeIntr(irq);
	}
	for (i = 0; pool->slots && i < pool->slot_count; ++i) {
		if (pool->slots[i].me_readers) {
			g_me_quarantined = 1U;
			return;
		}
	}

	if (pool->slots) {
		for (i = 0; i < pool->slot_count; ++i) {
			if (pool->slots[i].data) {
				if (!pool->slots[i].me_quarantined)
					free_64(pool->slots[i].data);
				pool->slots[i].data = 0;
			}

			pool->slots[i].capacity = 0;
			pool->slots[i].size = 0;
			pool->slots[i].timestamp = 0;
			pool->slots[i].state = PPA_PACKET_FREE;
			pool->slots[i].heap_fallback = 0;
		}

		free(pool->slots);
	}

	memset(pool, 0, sizeof(*pool));
}

struct ppa_packet_slot *ppa_packet_pool_acquire(struct ppa_packet_pool *pool,
                                                unsigned required_size) {
	unsigned i;
	unsigned rounded_size;

	if (!pool || !pool->slots) {
		note_acquire_fail(pool);
		return 0;
	}

	rounded_size = round64(required_size);

	for (i = 0; i < pool->slot_count; ++i) {
		if (pool->slots[i].state == PPA_PACKET_FREE &&
		    !pool->slots[i].me_readers && !pool->slots[i].me_quarantined &&
		    pool->slots[i].data != 0 &&
		    pool->slots[i].capacity >= rounded_size) {
			pool->slots[i].size = 0;
			pool->slots[i].timestamp = 0;
			pool->slots[i].state = PPA_PACKET_FILLING;
			pool->slots[i].heap_fallback = 0;

			note_acquire_success(pool, required_size);
			return &pool->slots[i];
		}
	}

	for (i = 0; i < pool->slot_count; ++i) {
		if (pool->slots[i].state == PPA_PACKET_FREE &&
		    !pool->slots[i].me_readers && !pool->slots[i].me_quarantined) {
			void *new_data;
			unsigned old_capacity = pool->slots[i].capacity;

			if (pool->slots[i].data != 0 &&
			    pool->slots[i].capacity >= rounded_size) {
				pool->slots[i].size = 0;
				pool->slots[i].timestamp = 0;
				pool->slots[i].state = PPA_PACKET_FILLING;
				pool->slots[i].heap_fallback = 0;

				note_acquire_success(pool, required_size);
				return &pool->slots[i];
			}

			new_data = malloc_64(rounded_size);

			if (!new_data) {
				note_acquire_fail(pool);
				return 0;
			}

			if (pool->slots[i].data)
				free_64(pool->slots[i].data);

			pool->slots[i].data = new_data;
			pool->slots[i].capacity = rounded_size;
			pool->slots[i].size = 0;
			pool->slots[i].timestamp = 0;
			pool->slots[i].state = PPA_PACKET_FILLING;
			pool->slots[i].heap_fallback = 0;

			note_grow(pool, old_capacity, rounded_size);
			note_acquire_success(pool, required_size);

			return &pool->slots[i];
		}
	}

	note_acquire_fail(pool);
	return 0;
}

void ppa_packet_pool_release(struct ppa_packet_pool *pool,
                             struct ppa_packet_slot *slot) {

	if (!slot)
		return;

	if (pool != 0) {

		if (pool->active_slots > 0)
			pool->active_slots--;
	}

	slot->size = 0;
	slot->timestamp = 0;
	slot->state = PPA_PACKET_FREE;
	slot->heap_fallback = 0;
}

void ppa_packet_pool_trim_free(struct ppa_packet_pool *pool,
                               unsigned floor_capacity,
                               struct ppa_packet_pool_trim_result *out)
{
	unsigned retain;
	unsigned far_above;
	unsigned i;
	struct ppa_packet_pool_trim_result result;

	memset(&result, 0, sizeof(result));
	if (pool == 0 || pool->slots == 0) {
		if (out != 0)
			*out = result;
		return;
	}

	retain = packet_pool_recent_p95(pool);
	if (retain < pool->slot_capacity)
		retain = pool->slot_capacity;
	if (retain < floor_capacity)
		retain = floor_capacity;
	retain = round64(retain);
	result.retain_capacity = retain;
	far_above = retain <= 0xffffffffU / 2U ? retain * 2U : 0xffffffffU;

	for (i = 0U; i < pool->slot_count; ++i) {
		struct ppa_packet_slot *slot = &pool->slots[i];
		unsigned capacity;

		if (slot->state != PPA_PACKET_FREE || slot->data == 0 ||
		    slot->me_readers || slot->me_quarantined)
			continue;
		capacity = slot->capacity;
		/* Avoid allocator churn for near-p95 buffers. A slot must be at least
		 * twice the retained size and waste a meaningful 64 KiB. */
		if (capacity <= far_above || capacity - retain < 64U * 1024U)
			continue;

		free_64(slot->data);
		slot->data = 0;
		slot->capacity = 0U;
		slot->size = 0U;
		slot->timestamp = 0U;
		slot->heap_fallback = 0U;
		result.slots_freed++;
		result.bytes_freed += capacity;
		if (pool->allocated_bytes >= capacity)
			pool->allocated_bytes -= capacity;
		else
			pool->allocated_bytes = 0U;
	}

	memset(pool->size_histogram, 0, sizeof(pool->size_histogram));
	pool->histogram_samples = 0U;
	
	if (out != 0)
		*out = result;
}
