#include "ppa_media_packet.h"

static void ppa_media_packet_copy(struct ppa_media_packet *dst,
                                  const struct ppa_media_packet *src)
{
    dst->size = src->size;
    dst->data = src->data;
    dst->timestamp = src->timestamp;
    dst->pool = src->pool;
    dst->slot = src->slot;
    dst->audio_sample_position = src->audio_sample_position;
    dst->audio_duration = src->audio_duration;
    dst->audio_flags = src->audio_flags;
    dst->audio_trim_start = src->audio_trim_start;
    dst->audio_trim_end = src->audio_trim_end;
}

void ppa_media_packet_reset(struct ppa_media_packet *packet)
{
    if (packet == 0)
        return;

    packet->size = 0;
    packet->data = 0;
    packet->timestamp = 0;
    packet->pool = 0;
    packet->slot = 0;
    packet->audio_sample_position = 0;
    packet->audio_duration = packet->audio_flags = 0;
    packet->audio_trim_start = packet->audio_trim_end = 0;
}

void ppa_media_packet_prepare(struct ppa_media_packet *packet,
                              struct ppa_packet_pool *pool,
                              struct ppa_packet_slot *slot,
                              unsigned int size)
{
    if (packet == 0 || slot == 0)
        return;

    ppa_media_packet_reset(packet);

    packet->size = size;
    packet->data = slot->data;
    packet->timestamp = 0;
    packet->pool = pool;
    packet->slot = slot;

    slot->size = size;
    slot->timestamp = 0;
    slot->state = PPA_PACKET_FILLING;
}

void ppa_media_packet_release(struct ppa_media_packet *packet)
{
    if (packet == 0)
        return;

    if (packet->slot != 0)
        ppa_packet_pool_release(packet->pool, packet->slot);

    ppa_media_packet_reset(packet);
}

int ppa_media_packet_ring_push(struct ppa_media_packet *storage,
                               unsigned int *size,
                               unsigned int *rear,
                               unsigned int capacity,
                               struct ppa_media_packet *packet)
{
    if (storage == 0 || size == 0 || rear == 0 || packet == 0 ||
        capacity == 0 || *size >= capacity)
        return 0;

    if (packet->slot != 0)
        packet->slot->state = PPA_PACKET_QUEUED;

    ppa_media_packet_copy(&storage[*rear], packet);
    /* The ring owns the pool slot after a successful push. */
    ppa_media_packet_reset(packet);
    *rear = (*rear + 1U) % capacity;
    ++*size;
    return 1;
}

int ppa_media_packet_ring_pop(struct ppa_media_packet *storage,
                              unsigned int *size,
                              unsigned int *front,
                              unsigned int capacity,
                              struct ppa_media_packet *packet)
{
    if (storage == 0 || size == 0 || front == 0 || packet == 0 ||
        capacity == 0 || *size == 0)
        return 0;

    ppa_media_packet_copy(packet, &storage[*front]);
    if (packet->slot != 0)
        packet->slot->state = PPA_PACKET_DECODING;

    ppa_media_packet_reset(&storage[*front]);
    *front = (*front + 1U) % capacity;
    --*size;
    return 1;
}

void ppa_media_packet_ring_clear(struct ppa_media_packet *storage,
                                 unsigned int *size,
                                 unsigned int *front,
                                 unsigned int *rear,
                                 unsigned int capacity)
{
    unsigned int i;

    if (storage != 0) {
        for (i = 0; i < capacity; ++i)
            ppa_media_packet_release(&storage[i]);
    }
    if (size != 0) *size = 0;
    if (front != 0) *front = 0;
    if (rear != 0) *rear = 0;
}
