# Playback standby

The power switch now uses the same controlled suspend path as the sleep timer.
The intended sequence is: pause the open movie, enter native PSP standby, wake
with the retained frame visible, then wait for a fresh **Square** press. Triangle
still stops after wake. Seeking and chapter controls cannot implicitly resume
the movie before Play. Buttons held during wake are discarded until released.

## Cause and change

`PpuPlayer::playMovie()` held `scePowerLock(0)` across the entire movie. The
session handled the sleep timer and actual suspend/resume callbacks, but did
not service a physical switch request while that lock was held. The request
therefore remained deferred until playback exited and released the lock.
[PSPSDK documents this deferred-switch behavior](https://pspdev.github.io/pspsdk/psppower_8h.html).

The callback still only posts flags. The playback controller now handles
`PSP_POWER_CB_POWER_SWITCH` and polls `scePowerIsRequest()` for a queued request
while locked. Its import is already exported by PSPSDK's `scePower.S`, NID
`0x7FA406DD`; an explicit declaration covers SDK headers that omit it.
`scePowerIsSuspendRequired()` is a battery predicate and is not used as the
switch detector. No additional privileged bridge export is required.

Before releasing the playback lock, the controller:

1. Pauses the movie and waits for decoder, video and audio workers to acknowledge
   their existing safe pause points. Audio-only playback omits the video worker.
2. Waits for channel 0's queued audio samples to drain. A failed query is not
   accepted as proof that the hardware is idle.
3. Parks the read-ahead pump after its current native read finishes, retaining
   file descriptors, queued ranges and partially completed request offsets.
   Parking happens after the decoder stops, so a decoder waiting for I/O can
   complete its work and acknowledge pause.
4. Parks the optional debug writer between drains, saves the visible framebuffer
   address/format, and flushes CPU caches.
5. Publishes lock ownership before unlocking. A timer request is queued before
   unlock; a physical switch request is already pending. No new suspend request
   is made after unlock, which can return only after a full sleep/wake cycle.

On `RESUME_COMPLETE`, playback restores its power lock, display surface and
timeline anchor. Wake wins over coalesced switch/suspend flags. Input must be
neutral after a short debounce before a fresh Square press can unpark I/O and
diagnostics, raise playback clocks and release the paused workers. Stop follows
the normal joined-worker teardown. Raw switch intent no longer puts the browser
into its suspended state if the native transition is denied.

Repeated pause acknowledgements also cover a rapid resume/pause transition in
which a worker never actually leaves its wait loop. Seek arrival preserves the
controller's current pause state instead of clearing standby acknowledgements.

## Memory and hardware ownership

The existing movie, queues, codec contexts, PCM and display buffers stay allocated.
There is no RAM snapshot file, duplicate frame pool or extra large allocation.
The added state consists of scalar flags/pointers and a pump flag, suitable for
the PSP-1000 memory budget. Standby itself is handled by the native firmware.
No custom ME/VME program is loaded, and no codec/ME reboot, reset or teardown
occurs during the transition. This relies on the native drivers restoring their
hardware state and must be checked on physical devices and the supported CFWs.

If a timer cannot safely park playback or a native request is cancelled/denied,
the existing paused/display-off fallback remains available. A still-pending
physical request is not abandoned and left to trigger unexpectedly on Stop.
A firmware call that never returns cannot be made safe by forcibly releasing
its buffers or terminating its owner. A forced shutdown or loss of power is not
standby and cannot preserve this in-memory session.

## Validation

Host regression tests run the real session controller, I/O pump loop and debug
writer against mocked PSP calls. AddressSanitizer/UndefinedBehaviorSanitizer
checks pass for pause acknowledgement, DMA/read/writer drain ordering, retained
read offsets, immediate sleep/wake inside unlock, lock failure retries,
coalesced callbacks, audio-only playback, one-shot timer behavior, restored
framebuffer and the fresh-Play gate. Existing diagnostics, memory rollback and
error-screen tests also pass. Native syntax checks pass against PSPSDK headers;
the host-only check of `main.cpp` skips the unchanged 32-bit ABI assertions.

These checks do not emulate Sony hardware. No PSP cross-build or physical
standby test was available here. Before release, test on PSP-1000 and PSP-3000:

| Case | Expected result |
| --- | --- |
| MP4 and MKV during normal playback | Switch sleeps promptly after current hardware work drains; wake shows the retained frame, paused. |
| Already paused; rapid pause/seek then switch | No hang, decoder error, implicit resume or browser exit. |
| Wake with Square or seek/chapter buttons held | Remains paused until release and a fresh Square press. |
| Repeated sleep/wake, including another sleep before Play | No extra suspend request or lock imbalance; same movie stays open. |
| Native AVC and H264X, AAC and software audio | Manual resume retains usable decoder/audio state and A/V timing. |
| FAT and NTFS media, subtitle sidecars, Debug Mode on/off | Reads/logging resume without corruption, stale handles or lost queued ranges. |
| Audio Only Mode and sleep timer | No wait for a nonexistent video worker; wake remains paused. |
| Stop or HOME after wake; subsequent movie and browser standby | Normal teardown; no delayed sleep on movie exit. |

Keep the card inserted across standby. Include CFW version, PSP model and Debug
Mode logs when reporting a device-specific failure.
