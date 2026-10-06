#ifndef PPA_MEDIA_PACKET_H
#define PPA_MEDIA_PACKET_H

#include <stdint.h>
#include "ppa_packet_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ppa_media_packet {
    unsigned int size;
    void *data;
    int timestamp;
    struct ppa_packet_pool *pool;
    struct ppa_packet_slot *slot;
    /* Optional source PCM timing for software codecs. Zero flags retain the
     * legacy AAC/MP3 packet contract. Counts are per channel, before SRC. */
    uint64_t audio_sample_position;
    uint32_t audio_duration;
    uint32_t audio_flags; /* 1 = absolute sample position/duration valid */
    uint32_t audio_trim_start, audio_trim_end;
};

void ppa_media_packet_reset(struct ppa_media_packet *packet);
void ppa_media_packet_prepare(struct ppa_media_packet *packet,
                              struct ppa_packet_pool *pool,
                              struct ppa_packet_slot *slot,
                              unsigned int size);
void ppa_media_packet_release(struct ppa_media_packet *packet);

int ppa_media_packet_ring_push(struct ppa_media_packet *storage,
                               unsigned int *size,
                               unsigned int *rear,
                               unsigned int capacity,
                               struct ppa_media_packet *packet);
int ppa_media_packet_ring_pop(struct ppa_media_packet *storage,
                              unsigned int *size,
                              unsigned int *front,
                              unsigned int capacity,
                              struct ppa_media_packet *packet);
void ppa_media_packet_ring_clear(struct ppa_media_packet *storage,
                                 unsigned int *size,
                                 unsigned int *front,
                                 unsigned int *rear,
                                 unsigned int capacity);

#ifdef __cplusplus
}
#endif

#endif
