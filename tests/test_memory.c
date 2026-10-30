#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "../ppu/common/ppa_memory.c"
#include "../ppu/mod/psp1k_frame_buffer.c"
const char *__asan_default_options(void) { return "detect_leaks=0"; }
static int fail_at, calls, live;
void *malloc_64(int size)
{
    if (++calls==fail_at) return 0;
    void *p=malloc(size);if(p)++live;return p;
}
void free_64(void *p) { if(p) { --live;free(p); } }
int sceKernelWaitSema(SceUID s,int n,SceUInt *t) { (void)s;(void)n;(void)t;return 0; }
int sceKernelSignalSema(SceUID s,int n) { (void)s;(void)n;return 0; }
int main(void)
{
    int i,j;
    for(i=1;i<=4;++i) {
        fail_at=i;calls=0;assert(!psp1k_init_frame_buffer());assert(live==0);
        for(j=0;j<4;++j) assert(!psp1k_get_frame_buffer(j));
    }
    fail_at=0;calls=0;assert(psp1k_init_frame_buffer());assert(live==4 && calls==4);
    assert(psp1k_init_frame_buffer());assert(live==4 && calls==4);
    assert(!psp1k_get_frame_buffer(-1) && !psp1k_get_frame_buffer(4));
    for(j=0;j<4;++j) { free_64(frame_buffers[j]);frame_buffers[j]=0; }
    g_memory.base=(unsigned char *)1;
    assert(!ppa_memory_large_alloc(0xffffffffU));
    assert(!ppa_memory_large_alloc(0xffffffc1U));
    g_memory.base=0;assert(live==0);
    puts("PASS: PSP-1000 framebuffer allocation failure rollback and arena size-overflow rejection");
    return 0;
}
