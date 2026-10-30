/* Host regression harness. Real diagnostic/state code, mocked PSP syscalls.
 * This does not emulate MIPS exceptions, ME/VME, CFW or card latency. */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../ppu/common/ppa_debug.c"
#include "../ppu/mod/subtitle_readability.h"
/* This runner has no /proc/task access; keep address/UB checks, disable LSan. */
const char *__asan_default_options(void) { return "detect_leaks=0"; }
static char root[256];
static int write_failure, short_write, start_failure, writes, opens;
static int logger_park_test, park_writes;
static uint64_t clock_us = 1000000;
static struct ppa_session_snapshot sample;
static const char *local_path(const char *p)
{
    static char out[512];
    assert(!strncmp(p, "ms0:", 4));
    snprintf(out, sizeof(out), "%s%s", root, p+4);
    return out;
}
SceUID sceIoOpen(const char *p, int flags, SceMode mode)
{
    int f = (flags & PSP_O_WRONLY) ? O_WRONLY : O_RDONLY;
    if (flags & PSP_O_CREAT) f |= O_CREAT;
    if (flags & PSP_O_TRUNC) f |= O_TRUNC;
    if (flags & PSP_O_APPEND) f |= O_APPEND;
    ++opens;
    return open(local_path(p), f, mode);
}
int sceIoClose(SceUID f) { return close(f); }
int sceIoRead(SceUID f, void *p, SceSize n) { return read(f,p,n); }
int sceIoWrite(SceUID f, const void *p, SceSize n)
{
    ++writes;
    if (write_failure) return -1;
    if (short_write && n > 3) n=3;
    return write(f,p,n);
}
SceOff sceIoLseek(SceUID f, SceOff o, int whence) { return lseek(f,o,whence); }
int sceIoMkdir(const char *p, SceMode mode) { return mkdir(local_path(p),mode); }
int sceIoRemove(const char *p) { return unlink(local_path(p)); }
int sceIoRename(const char *p, const char *q)
{
    char a[512]; snprintf(a,sizeof(a),"%s",local_path(p));
    return rename(a,local_path(q));
}
int sceIoSync(const char *p, unsigned int flags) { (void)p; (void)flags; return 0; }
unsigned int sceKernelCpuSuspendIntr(void) { return 0; }
void sceKernelCpuResumeIntr(unsigned int state_) { (void)state_; }
SceInt64 sceKernelGetSystemTimeWide(void) { return clock_us; }
SceUID sceKernelGetThreadId(void) { return 20; }
int sceKernelSleepThread(void) { abort(); }
int sceKernelDelayThread(SceUInt n)
{
    if (logger_park_test == 1) {
        assert(n == 5000 && logger_park == 2 && writes == park_writes);
        assert(ppa_debug_park());
        /* Re-arm before the writer runs: a stale acknowledgement must not
         * permit the next standby while the writer is draining records. */
        ppa_debug_unpark();
        assert(!ppa_debug_park());
        ppa_debug_unpark();
        logger_park_test = 2;
    } else if (logger_park_test == 2) {
        assert(n == 100000 && writes > park_writes);
        stop_logger = 1; logger_park_test = 0;
    }
    return 0;
}
SceUID ppa_thread_create(enum ppa_thread_role r, const char *s, SceKernelThreadEntry e, int stack, unsigned int attrs)
{ (void)r;(void)s;(void)e;(void)attrs;assert(stack==8192);return 1000; }
int sceKernelStartThread(SceUID t,SceSize n,void *p) { (void)t;(void)n;(void)p;return start_failure ? -1 : 0; }
int sceKernelDeleteThread(SceUID t) { (void)t;return 0; }
int ppa_wait_thread_end_safe(SceUID t,const char *a,const char *b,unsigned int c,unsigned int d)
{ (void)t;(void)a;(void)b;(void)c;(void)d;drain();return 0; }
void ppa_wait_quarantine(const char *s,int e) { (void)s;(void)e;abort(); }
int pspDebugInstallErrorHandler(PspDebugErrorHandler h) { assert(h);return 0; }
void _pspDebugExceptionHandler(void) {}
int cooleyesDebugExceptionHandler(void *p,int on) { (void)p;(void)on;return 0; }
unsigned int ppa_privileged_bridge_capabilities(void) { return COOLEYES_BRIDGE_CAP_DEBUG_EXCEPTION; }
int scePowerGetCpuClockFrequencyInt(void) { return 222; }
int scePowerGetBusClockFrequencyInt(void) { return 111; }
SceSize sceKernelTotalFreeMemSize(void) { return 123456; }
SceSize sceKernelMaxFreeMemSize(void) { return 65432; }
int sceKernelDevkitVersion(void) { return 0x06060110; }
int sceRtcGetCurrentClockLocalTime(ScePspDateTime *t) { memset(t,0,sizeof(*t));t->year=2026;return 0; }
void ppa_hardware_profile_refresh_memory(void) {}
const struct ppa_hardware_profile *ppa_hardware_profile_get(void)
{ static struct ppa_hardware_profile hw={0}; hw.expected_physical_ram_mb=32;hw.ge_edram_bytes=2097152;return &hw; }
const char *ppa_hardware_model_name(void) { return "PSP-1000"; }
unsigned int ppa_session_audio_only_queued_blocks(void) { return 2; }
int ppa_session_debug_snapshot(struct ppa_session_snapshot *s) { *s=sample; return 1; }
static void reboot(void)
{
    ppa_debug_shutdown(); movie=started=0; fault_ready=0;
    ppa_debug_init(1);
}
static void play(void)
{
    ppa_debug_movie_begin("ms0:/movie.mkv","mkv",0);
    ppa_debug_movie_started();
    ppa_debug_movie_end("exit: manual");
}
static int contains(const char *path,const char *needle)
{
    char buf[8192]; int n; SceUID fd=sceIoOpen(path,PSP_O_RDONLY,0);
    assert(fd>=0); n=sceIoRead(fd,buf,sizeof(buf)-1);sceIoClose(fd);assert(n>=0);buf[n]=0;
    return strstr(buf,needle)!=NULL;
}
int main(void)
{
    struct ppa_debug_state a={0},b;
    char tmp[256];
    unsigned int i;
    snprintf(tmp,sizeof(tmp),"%s/ppu-debug-tests-XXXXXX",getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    assert(mkdtemp(tmp)); snprintf(root,sizeof(root),"%s",tmp);
    ppa_debug_init(0); assert(!ppa_debug_enabled());
    i=writes;ppa_debug_event("off");ppa_debug_tick(1);assert(writes==(int)i);
    short_write=1; assert(ppa_debug_set_enabled(1));short_write=0;
    assert(ppa_debug_remaining()==3 && ppa_debug_enabled());
    ppa_debug_movie_begin("bad.mkv","mkv",0);ppa_debug_movie_end("open failed");
    assert(ppa_debug_remaining()==3); /* open failure is not a play */
    ppa_debug_movie_begin("start.mkv","mkv",0);ppa_debug_movie_started();
    ppa_debug_movie_start_failed();ppa_debug_movie_end("worker start failed");
    assert(ppa_debug_remaining()==3);
    play();assert(ppa_debug_remaining()==2);reboot();assert(ppa_debug_remaining()==2);
    play();assert(ppa_debug_remaining()==1);reboot();assert(ppa_debug_enabled());
    play();assert(!ppa_debug_enabled() && !ppa_debug_remaining());
    reboot();assert(!ppa_debug_enabled()); /* stale config TRUE cannot refill */
    assert(ppa_debug_set_enabled(1));assert(ppa_debug_remaining()==3);
    assert(ppa_debug_set_enabled(1));assert(ppa_debug_remaining()==3); /* explicit rearm */
    for(i=0;i<3;++i) {
        ppa_debug_movie_begin("crash.mkv","mkv",0);ppa_debug_movie_started();reboot();
        assert(ppa_debug_remaining()==2-i);
    }
    assert(!ppa_debug_enabled());
    assert(contains(DEBUG_CRASH,"previous playback interrupted"));
    assert(ppa_debug_set_enabled(1));
    ppa_debug_movie_begin("quote,\"name\".mkv","mkv",0);
    ppa_debug_movie_started();
    memset(&sample,0,sizeof(sample));sample.presented=10;sample.decode_samples=10;
    sample.decode_total_us=20000;sample.decode_peak_us=3000;sample.budget_us=33333;
    ppa_debug_tick(0);i=head;ppa_debug_tick(0);assert(head==i); /* <=1 Hz */
    clock_us+=1000000;sample.paused=1;ppa_debug_tick(0);i=head;
    clock_us+=1000000;ppa_debug_tick(0);assert(head==i); /* paused dedup */
    clock_us+=10000000;ppa_debug_tick(0);assert(head!=i);
    { struct h264x_au_info au={0};au.valid=1;au.is_b=au.is_reference_b=1;
      ppa_debug_note_au(&au);ppa_debug_note_au(&au);assert(b_count==2 && ref_b_count==2 && max_b_run==2);
      ppa_debug_discontinuity();ppa_debug_note_au(&au);assert(max_b_run==2);
      au.parse_error=1;ppa_debug_note_au(&au);assert(parse_failures==1); }
    { unsigned char sei[128]={0};const char *options="x264 - options: bframes=3 b_pyramid=1";
      unsigned int n=strlen(options),payload=16+n,nal=1+2+payload+1;
      sei[3]=nal;sei[4]=6;sei[5]=5;sei[6]=payload;memcpy(sei+23,options,n);sei[23+n]=0x80;
      encoder_probes=0;ppa_debug_encoder_options(sei,nal+4,4);drain();
      assert(contains(DEBUG_CRASH,"bframes=3")); assert(contains(DEBUG_CRASH,"b_pyramid=1"));
      for(i=0;i<nal+4;++i) { encoder_probes=0;ppa_debug_encoder_options(sei,i,4); }
      for(i=0;i<2000;++i) { unsigned j; for(j=0;j<sizeof(sei);++j) sei[j]=rand();
        encoder_probes=0;ppa_debug_encoder_options(sei,rand()%sizeof(sei),rand()%7); }
    }
    for(i=0;i<100;++i) ppa_debug_error("bounded overflow",-123);
    assert(dropped>0);drain();
    fault_regs.epc=0x08801234;fault_regs.cause=16;fault_thread=42;fault_ready=2;drain();
    assert(contains(DEBUG_CRASH,"EPC=08801234"));
    ppa_debug_event("retained across standby");
    park_writes = writes; logger_park_test = 1;
    assert(!ppa_debug_park());
    assert(logger_main(0, NULL) == 0);
    assert(!logger_park_test && head == tail);
    assert(contains(DEBUG_CRASH,"retained across standby"));
    stop_logger = 0;
    ppa_debug_movie_end(NULL);ppa_debug_shutdown();
    write_failure=1;assert(!ppa_debug_set_enabled(1));assert(!ppa_debug_enabled());write_failure=0;
    a.enabled=1;a.remaining=3;a.generation=1;ppa_debug_state_seal(&a);assert(ppa_debug_state_valid(&a));
    b=a;b.remaining=2;assert(!ppa_debug_state_valid(&b));
    b=a;b.generation=2;ppa_debug_state_seal(&b);assert(ppa_debug_state_newer(&b,&a));
    b.remaining=4;ppa_debug_state_seal(&b);assert(!ppa_debug_state_valid(&b));
    assert(!ppu_subtitle_needs_detail("Latin café Ελληνικά Русский"));
    assert(ppu_subtitle_needs_detail("日本語 中文 한국어 العربية עברית हिन्दी ไทย"));
    assert(ppu_subtitle_needs_detail("Latin + اردو"));
    assert(!ppu_subtitle_needs_detail("\xf0\x80\x80\x80\xff\xe0"));
    assert(ppu_subtitle_readable_size(16,0)==16 && ppu_subtitle_readable_size(16,1)==20);
    assert(ppu_subtitle_readable_size(24,1)==24);
    assert(ppu_subtitle_readable_size(1,0)==1 && ppu_subtitle_readable_size(48,0)==48);
    puts("PASS: persistent allowance, relaunch/crash, failed starts/writes, bounded telemetry, SEI, exception mailbox, subtitle scripts");
    return 0;
}
