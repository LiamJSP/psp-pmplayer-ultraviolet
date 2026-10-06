#ifndef PPA_CACHE_H
#define PPA_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPA_CACHE_LINE 64U
#define PPA_UNCACHED_MASK 0x40000000U

static inline void *ppa_uncached_ptr(void *p) {
	return (void *)(PPA_UNCACHED_MASK | (uintptr_t)p);
}

static inline const void *ppa_uncached_cptr(const void *p) {
	return (const void *)(PPA_UNCACHED_MASK | (uintptr_t)p);
}

static inline const void *ppa_cached_cptr(const void *p) {
	return (const void *)((uintptr_t)p & ~((uintptr_t)PPA_UNCACHED_MASK));
}

uintptr_t ppa_cache_align_down(uintptr_t p);
unsigned  ppa_cache_aligned_size(const void *p, unsigned size);

void ppa_cache_wb_range(const void *p, unsigned size);
void ppa_cache_inv_range(const void *p, unsigned size);
void ppa_cache_wbinv_range(const void *p, unsigned size);
void ppa_cache_wbinv_all(void);

/*
 * CachePolicy vocabulary:
 *
 * CPU wrote, GE will read texture/frame data:
 *   ppa_cache_cpu_wrote_ge_will_read()
 *
 * CPU wrote, ME/VME/media engine will read:
 *   ppa_cache_cpu_wrote_me_will_read()
 *
 * GE/ME wrote, CPU will read:
 *   ppa_cache_device_wrote_cpu_will_read()
 *
 * Ambiguous shared ownership transition:
 *   ppa_cache_shared_transition()
 */
void ppa_cache_cpu_wrote_ge_will_read(const void *p, unsigned size);
void ppa_cache_cpu_wrote_me_will_read(const void *p, unsigned size);
void ppa_cache_device_wrote_cpu_will_read(const void *p, unsigned size);
void ppa_cache_shared_transition(const void *p, unsigned size);

#ifdef __cplusplus
}
#endif

#endif