#include "ppa_cache.h"
#include "ppa_bus_manager.h"

#include <pspkernel.h>
#include <psputils.h>

uintptr_t ppa_cache_align_down(uintptr_t p) {
	return p & ~((uintptr_t)PPA_CACHE_LINE - 1U);
}

unsigned ppa_cache_aligned_size(const void *p, unsigned size) {
	uintptr_t start = ppa_cache_align_down((uintptr_t)p);
	uintptr_t end = ((uintptr_t)p + size + PPA_CACHE_LINE - 1U) &
	                ~((uintptr_t)PPA_CACHE_LINE - 1U);

	return (unsigned)(end - start);
}

void ppa_cache_wb_range(const void *p, unsigned size) {
	if (p == 0 || size == 0)
		return;

	{
		const void *start = (const void *)ppa_cache_align_down((uintptr_t)p);
		unsigned aligned_size = ppa_cache_aligned_size(p, size);
		struct ppa_bus_token bus_token = ppa_bus_begin(PPA_BUS_CACHE);
		sceKernelDcacheWritebackRange((void *)start, aligned_size);
		ppa_bus_note_cache_bytes(aligned_size);
		ppa_bus_end(&bus_token);
	}
}

void ppa_cache_inv_range(const void *p, unsigned size) {
	if (p == 0 || size == 0)
		return;

	{
		const void *start = (const void *)ppa_cache_align_down((uintptr_t)p);
		unsigned aligned_size = ppa_cache_aligned_size(p, size);
		struct ppa_bus_token bus_token = ppa_bus_begin(PPA_BUS_CACHE);
		sceKernelDcacheInvalidateRange((void *)start, aligned_size);
		ppa_bus_note_cache_bytes(aligned_size);
		ppa_bus_end(&bus_token);
	}
}

void ppa_cache_wbinv_range(const void *p, unsigned size) {
	if (p == 0 || size == 0)
		return;

	{
		const void *start = (const void *)ppa_cache_align_down((uintptr_t)p);
		unsigned aligned_size = ppa_cache_aligned_size(p, size);
		struct ppa_bus_token bus_token = ppa_bus_begin(PPA_BUS_CACHE);
		sceKernelDcacheWritebackInvalidateRange((void *)start, aligned_size);
		ppa_bus_note_cache_bytes(aligned_size);
		ppa_bus_end(&bus_token);
	}
}

void ppa_cache_wbinv_all(void) {
	{
		struct ppa_bus_token bus_token = ppa_bus_begin(PPA_BUS_CACHE);
		sceKernelDcacheWritebackInvalidateAll();
		ppa_bus_note_cache_bytes(32U * 1024U);
		ppa_bus_end(&bus_token);
	}
}

void ppa_cache_cpu_wrote_ge_will_read(const void *p, unsigned size) {
	ppa_cache_wb_range(p, size);
}

void ppa_cache_cpu_wrote_me_will_read(const void *p, unsigned size) {
	ppa_cache_wb_range(p, size);
}

void ppa_cache_device_wrote_cpu_will_read(const void *p, unsigned size) {
	ppa_cache_inv_range(p, size);
}

void ppa_cache_shared_transition(const void *p, unsigned size) {
	ppa_cache_wbinv_range(p, size);
}
