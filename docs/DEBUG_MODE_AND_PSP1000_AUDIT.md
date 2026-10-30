# Debug Mode, pause fixes, and PSP-1000 audit

This source revision adds optional diagnostics, fixes pause/clock ordering and
stale stall timers, and makes targeted allocation and subtitle changes. The
reported hardware-decoder failure has **not** been reproduced here. The pause
changes address concrete races in the source; they are not proof that every
reported failure has the same cause. No new EBOOT or PRX was produced in this
environment, and these changes still need testing on real PSP hardware.

## Enable and collect diagnostics

In Configuration, set **Debug Mode** to **True** and confirm. The shipped default
is **False**. Confirming True again replenishes the allowance to three plays.
Diagnostics are independent of the existing Playback Health overlay.

Files are written to the FAT memory-card path `ms0:/PSP/PPU_DEBUG/`, including
when the movie comes from the NTFS reader:

| File | Contents and limit |
| --- | --- |
| `crash.log` | Session identity, hardware, video metadata, pause events, codec errors and best-effort CPU exception dump; 256 KiB maximum |
| `crash.previous.log` | One previous log, rotated when a new capture starts with the current log at least 128 KiB |
| `playback-1.csv` through `playback-3.csv` | One metrics file per allowance slot, up to 2 MiB each; slots are reused after explicit re-enabling |
| `state0.dat`, `state1.dat` | Alternating checksummed countdown records, synchronized at session boundaries |

Copy the logs before re-enabling if you want to preserve all three CSVs. A
log that reaches its cap stops growing. The logger stops writing a file after
a write failure instead of retrying continuously. An unavailable card prevents
Debug Mode from enabling; failure to persist the allowance disables capture.

A play is reserved immediately before playback workers start. Stopping or
finishing the third play disables Debug Mode after that session's final log.
The remaining allowance survives application restarts. A file-open failure
does not count; a worker-start failure refunds the reservation after any
workers that did start have joined safely. Looping or seeking within one open
movie remains one play. Audio Only Mode also counts the opened movie.

A crash, forced exit, or power loss consumes the reserved play. On the next
launch, a pending record reports that the previous playback was interrupted.
This deliberately prevents repeated crashes from leaving diagnostics enabled
forever. The marker cannot distinguish a crash from battery removal. A stale
True in XML, missing/corrupt state, or an exhausted allowance never grants a
new three-play allowance; confirm True in Configuration to start again.

## What the logs measure

Normal playback produces at most one scheduled CSV sample per second. An
unchanged paused session emits only a ten-second heartbeat. Session close can
emit one additional final sample. The CSV includes clocks, audio/video time,
presented/skipped frame deltas, sampled decode mean/peak and frame budget,
audio-only queue occupancy, user-partition free/largest block, compatibility
reference limits, and observed H.264 picture counts. `time_us` is monotonic
time since boot; the session header separately records local calendar time.
The audio queue metric is the application's audio-only queue, not the hardware
DMA queue. Partition free memory is **not** free space within the already
reserved newlib heap; it cannot diagnose heap fragmentation by itself.

The source SPS/PPS records include profile and level IDs, constraints, coded
and cropped dimensions, chroma format, bit depths, progressive/interlaced
flag, reference-frame limit, CABAC, weighted prediction, and 8x8 transform
flag. Container metadata adds frame-rate numerator/denominator, duration,
dimensions, audio codec, sample rate, channels, and battery-saver state.
Original SPS/PPS values are captured before compatibility rewriting.

H.264 does not have a universal SPS field for an encoder's `bframes` or
`b_pyramid` option. A bounded inspection of early user-data SEI records reports
those settings when the encoder includes them. Otherwise:

- Reference B pictures are reported as evidence of pyramid-style referencing.
- The maximum observed consecutive B-picture run is a lower bound observed
  during this playback, not a claim about the encoder's configured maximum.
- No observed reference B picture means **not observed**, not **disabled**.
- Seeking resets the current run so disconnected portions are not combined.
- Extreme Battery Saver bypasses normal AU inspection. The log marks its
  B-picture observations and SEI settings as potentially unavailable.

No metadata observation changes decoder compatibility policy.

## Performance and exception handling

Playback threads enqueue bounded records without doing log-file I/O. A fixed
32-slot ring uses about 12 KiB; overflow drops records and increments a counter.
A background-priority writer with an 8 KiB stack drains records, waking every
100 ms. The optional SEI scan examines at most 32 AUs and the first 4 KiB of
each SEI NAL. There are no per-frame logging allocations. The existing session
statistics use a nonblocking snapshot; missed samples are preferable to
blocking playback. Card writes still compete with movie reads, so overhead is
bounded but **not claimed to be zero or measured on a PSP**. State writes and
CSV creation occur at session boundaries, outside active decoding.

Ordinary diagnostic errors include Sony AVC/audio return codes, bridge ME
boot failure, semaphore errors, stalled joins, quarantine, and playback errors
returned to the browser. This is targeted error instrumentation, not an
interceptor for every possible libc or firmware failure.

For CPU exceptions, PSPSDK's exception assembly and user-mode trap remain in
the EBOOT. The kernel bridge exposes only registration/removal of that handler.
The callback copies registers into a preallocated mailbox and sleeps forever;
the already-running writer writes the dump. The callback performs no file I/O,
allocation, GU work, or codec reset. It never returns into PSPSDK's default
thread-deletion tail. Faulted thread stacks and possibly hardware-owned buffers
remain alive pending a power cycle. A failed handler detach also retains code
instead of unloading an address still registered with the kernel.

This is best-effort collection, **not crash recovery**. A destroyed stack,
scheduler/interrupt failure, kernel fault, ME/VME fault or hang, fault in the
writer, removed/full card, or abrupt power loss can prevent a dump. A pending
playback marker may be all that remains. Faults before configuration/bridge
initialization are outside the capture window. Restart the PSP after a fatal
exception; do not treat a still-responsive screen as evidence of safe recovery.

Build and deploy the matching EBOOT **and** `cooleyesBridge.prx` together. The
bridge API is now `0x00030002`, with an explicit exception capability. Host tests
exercise the mailbox and persistence logic, not MIPS exception entry, NID
resolution, CFW behavior, or the target link.

## Why pause handling changed

Previously, the control path lowered CPU/bus clocks immediately upon pausing,
while a decoder or output worker could still be inside a Sony call. Resume
also exposed the run flag before the boost completed. Memory pressure could
widen those windows on a smaller model, but the races were not model-specific.

The controller now waits for audio, video, and decode workers to acknowledge
their pause points, and for queued hardware audio to drain, before requesting
pause clocks. Audio-only sessions omit the absent video worker. Resume boosts
before clearing the shared pause flag; seek/chapter commands use that same
ordering. Old idle acknowledgements are cleared before another pause can use
them. Stop while paused restores normal clocks before joining/closing owners.

Two watchdog problems were also corrected. MP4 now excludes paused wall time,
including the iteration that resumes, from its stall deadline. MKV refreshes
its progress deadline when the decoder actually returns from a pause wait;
the old test after that wait normally saw the pause flag already cleared.
Neither path resets the ME, kills a worker inside firmware, or frees an active
decoder's buffers to escape an error.

## PSP-1000 audit

The application uses Sony's AVC/audio services and main-CPU H264X parsing and
rewriting. The inspected code does not install arbitrary application work on
the ME/VME. Main-CPU VFPU kernels are separate from ME ownership. Adding a
second ME runtime while these codecs are active would require a separate
ownership design; it is not introduced by diagnostics.

Model/datecode information is not used to assume an address is available.
Existing model queries, actual user-partition visibility, and successful fixed
region reservation remain the gates for extended surfaces. The diagnostic
header records the runtime `sceGeEdramGetSize()` result. GE-visible eDRAM and
firmware decoder working memory are treated separately; this revision never
assumes that a newer model provides four usable MiB for GE surfaces.

| Area / source | Finding and action |
| --- | --- |
| `ppu/mod/psp1k_frame_buffer.c` | Four 512×272×4 main-RAM surfaces use 2,228,224 bytes. Partial startup allocation previously retained earlier buffers after failure. Initialization now rolls back all four and supports a clean retry. These are not GE-eDRAM allocations. |
| `ppu/common/ppa_memory.c` | A near-`UINT_MAX` allocation could wrap 64-byte alignment. Requests above `0xffffffc0` are now rejected before rounding. Existing fixed-address reservation and model gates are retained. |
| `ppu/mod/mkv_play.c` | Incomplete optional staging/restoration pairs retained unusable heap allocations. Failed pairs are now released before playback. A third staging surface is also released when its required pair is absent. Existing null-pointer fallbacks remain in force. |
| `ppu/mod/gu_draw.c` | The PSP-1000 LCD layout is 557,056 bytes of draw surface plus at most 768×480×4 = 1,474,560 bytes of CSC source: 2,031,616 bytes, leaving 65,536 bytes inside 2 MiB. Three-surface direct scanout uses 1,671,168 bytes only when the RGB source is in successfully reserved extended main RAM. No new eDRAM surface was added. |
| `ppu/main.cpp`, `ppu/Makefile.3xx`, `ppu/mod/mp4avcdecoder.c` | **Remaining risk:** the default heap is 18 MiB, shared by many consumers. The 2 MiB DDRTOP allocation requires 4 MiB alignment and is retained across seeks/sessions. Alignment and fragmentation can prevent allocation despite sufficient total free bytes. Changing firmware arena placement/lifetime without device evidence is unsafe; it is unchanged. |
| `ppu/mod/mp4_file.c`, `libmp4info.psp/mp4info_type.h` | **Remaining risk:** index/sample structures permit up to 12 MiB and parser metadata up to 8 MiB, with transient overlap during open. These are independent ceilings, not a combined memory budget. Large/long MP4s can exhaust the smaller model's heap before playback. An adaptive index or aggregate budget needs representative files and separate implementation/testing. |
| `ppu/mod/subtitle_parse.h`, subtitle parsers | **Remaining risk:** each cue contains a 1,024-byte text array plus bookkeeping; up to 48 tracks may be opened and there is no aggregate cue-memory budget. Long/many sidecars and accumulated embedded cues can compete with decode buffers. A lazy subtitle loader or total budget remains follow-up work. |
| `ppu/mod/mkv_play.c`, H264X optional surfaces | **Remaining risk:** up to six optional LCD surfaces can consume another 3,342,336 bytes. Cleanup now avoids unusable partial pairs, but successful allocations still compete with fonts, subtitles and audio. Changing which restoration features are admitted on PSP-1000 needs playback/quality measurements. |
| Readers, NTFS, browser cache | Existing smaller reader queue ceilings, bounded raw NTFS reads/cache, and browser cache release for playback were retained. These reduce pressure but do not establish a whole-application peak-memory bound. |

This is a source-level audit of playback, memory layout, allocation failure,
thread ownership, storage and subtitle paths, not an exhaustive proof that all
inputs fit on PSP-1000. The remaining risks above are deliberately reported
instead of changing format admission or firmware resource lifetimes broadly.

## Subtitle defaults

Subtitle cues containing CJK/Kana/Hangul, Hebrew/Arabic, Indic, Thai/Lao,
Tibetan, Myanmar, Khmer and covered related scripts receive a minimum 20 px
glyph size and a 24 px line advance. A larger configured size remains larger,
with four pixels of line spacing for these scripts. UTF-8 cue contents drive
the choice, so missing or inaccurate track language tags do not prevent it.
Mixed Latin/detailed-script cues use the larger metrics for the entire cue.
Latin-only, Greek and Cyrillic cues retain their configured size and previous
line-spacing behavior. Interface fonts are unchanged. Invalid UTF-8 is handled
without reading beyond the string. Geometry is prepared before measuring and
placing a cue; font caches are invalidated only when effective metrics change.

## Validation and device checklist

Three native regression suites run real changed C functions with PSP syscalls
mocked. They passed AddressSanitizer and UndefinedBehaviorSanitizer checks:

- Pause acknowledgements, queued-audio drain, resume publication order, rapid
  toggles, audio-only behavior and the pause-wait return contract.
- Countdown across launches, explicit re-enable, failed opens/starts,
  interrupted sessions, stale XML, checksums, short/failed writes, log bounds,
  sampling cadence, malformed SEI inputs, B-picture observations, exception
  mailbox output, UTF-8 script classification and size selection.
- Failure at each PSP-1000 framebuffer allocation, rollback/retry, valid
  reuse, index bounds and large-allocation rounding rejection.

Run from the project root using a native compiler and installed PSPSDK headers:

```sh
PSPSDK_INCLUDE=/path/to/psp/sdk/include tests/run_host_tests.sh
```

The harness disables LeakSanitizer because this validation environment cannot
inspect `/proc` tasks; allocation rollback tests maintain explicit live-block
counts. These tests do not execute Allegrex code, Sony codecs, the kernel
bridge, the real scheduler, Memory Stick latency, or rendered subtitles.

Native syntax checks passed for 19 changed C translation units and the C++
configuration, menu and player units using PSPSDK headers. Font checking
excludes optional HarfBuzz/FriBidi build paths. Existing string-literal warnings
remain in the player. The PSP cross toolchain is absent here, so a complete
target compile/link was not possible; the platform-specific `main.cpp` build
contract was not validated with a native 64-bit compiler.

Before publishing binaries, build the complete application and bridge, then
test both PSP-1000 and PSP-3000 with the same CFW and media:

1. Baseline/Main and H264X MP4/MKV, short and >8-second pauses, rapid toggles,
   pause near EOF, paused stop, paused seek/chapter jump, suspend/resume, and
   Audio Only Mode. Verify no premature downclock or stale stall alarm.
2. Three starts/stops split across application launches, natural EOF, loop,
   failed open, explicit True re-confirmation, and an interrupted second/third
   play. Verify the state files and final automatic disable.
3. Card-full/unavailable failure paths and CSV caps. Use a development build
   to inject a controlled main-CPU exception; verify a register dump and cold
   restart behavior. Do not inject a fault into an active ME firmware call.
4. Long MP4s, many subtitle tracks, H264X restoration, and software audio while
   recording both heap usage and partition usage. Compare debug-off/on frame
   timing on the same card; do not infer overhead from host tests.
5. Latin, mixed CJK, Arabic and Indic multiline cues, including fallback fonts,
   user-selected larger fonts and TV-out where supported. Check clipping,
   baselines and shaping on the actual 480×272 display.

Primary API references used for the ownership/exception review:
[PSPSDK debug API](https://github.com/pspdev/pspsdk/blob/master/src/debug/pspdebug.h),
[exception trap implementation](https://github.com/pspdev/pspsdk/blob/master/src/debug/exception.c),
[exception assembly](https://github.com/pspdev/pspsdk/blob/master/src/debug/exception_asm.S),
[GE API](https://github.com/pspdev/pspsdk/blob/master/src/ge/pspge.h).
