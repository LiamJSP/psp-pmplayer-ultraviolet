/* Run the real pump loop against deterministic host I/O/semaphore callbacks.
 * No PSP driver calls or asynchronous host threads are involved. */
#include <assert.h>
#include <stdio.h>
#include "../ppu/common/ppa_io_pump.c"
const char *__asan_default_options(void) { return "detect_leaks=0"; }
static struct PpaIoPump pump;
static struct PpaIoRequest request;
static unsigned char bytes[8];
static int reads, waits, poll_error, mode;
static uint64_t clock_us = 1000000;
SceInt64 sceKernelGetSystemTimeWide(void) { return clock_us; }
int sceKernelPollSema(SceUID id, int n) { (void)id; (void)n; return poll_error ? -1 : 0; }
int sceKernelSignalSema(SceUID id, int n) { (void)id; (void)n; return 0; }
int sceKernelWaitSema(SceUID id, int n, SceUInt *timeout)
{
    (void)n; (void)timeout;
    if (id == pump.wake) {
        ++waits;
        if (pump.parked) {
            assert(pump.active == 0 && pump.queue_count == 1);
            assert(request.state == PPA_IO_REQUEST_QUEUED);
            assert(request.completed_bytes == (mode == 1 ? 0U : 4U));
            assert(ppa_io_pump_park(&pump));
            assert(ppa_io_pump_unpark(&pump));
        } else {
            assert(request.state == PPA_IO_REQUEST_COMPLETE);
            pump.stop = 1;
        }
    }
    return 0;
}
void ppa_io_set_cancel_check(ppa_io_cancel_check_fn fn, void *p) { (void)fn; (void)p; }
uint32_t ppa_vfs_recommended_burst(ppa_vfs_file *f) { (void)f; return 4; }
int ppa_vfs_worker_read_at64(ppa_vfs_file *f, uint64_t offset, void *dst,
                             uint32_t size, int async, int *fallback, int *seek)
{
    (void)f; (void)async; (void)fallback; (void)seek;
    assert(pump.active == &request && !pump.parked && size == 4);
    if (mode == 2 && reads == 0) {
        /* A controller may request parking while a native read is active.
         * It must get false until that burst returns and retires. */
        assert(!ppa_io_pump_park(&pump) && pump.parked);
    }
    memset(dst, (int)(offset + 1), size); ++reads; return size;
}
static void run(int scenario)
{
    struct PpaIoPump *arg = &pump;
    memset(&pump, 0, sizeof(pump)); memset(&request, 0, sizeof(request));
    memset(bytes, 0, sizeof(bytes)); reads = waits = 0; mode = scenario;
    pump.lock = 1; pump.wake = 2;
    request.file = (ppa_vfs_file *)0x1234;
    request.destination = bytes; request.bytes = sizeof(bytes);
    request.state = PPA_IO_REQUEST_QUEUED; request.completion_sema = 3;
    pump.queue[0] = &request; pump.queue_count = 1;
    poll_error = 1; assert(!ppa_io_pump_park(&pump) && !pump.parked);
    poll_error = 0;
    if (scenario == 1) assert(ppa_io_pump_park(&pump));
    assert(ppa_io_pump_thread(sizeof(arg), &arg) == 0);
    assert(reads == 2 && waits == 2 && request.result == 8 && request.completed_bytes == 8);
    assert(bytes[0] == 1 && bytes[3] == 1 && bytes[4] == 5 && bytes[7] == 5);
    assert(!request.cancel_requested && !pump.active && !pump.queue_count);
}
int main(void)
{
    assert(ppa_io_pump_park(NULL) && ppa_io_pump_unpark(NULL));
    run(1); run(2);
    puts("PASS: read-ahead parking, in-flight burst completion, queued range retention, nonblocking lock retry and resume without rereading");
    return 0;
}
