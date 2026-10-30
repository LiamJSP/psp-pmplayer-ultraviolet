# On-device error screens

Playback failures and Battery Saver rejections use persistent, paginated
screens. Text is drawn at twice the old emergency font size (10x14 pixels,
12-pixel character advance, 18-pixel line advance). Each page has up to nine
wrapped lines of 36 characters. Long filenames and internal error identifiers
wrap rather than running off the screen. Left/Right changes pages; X or Triangle
returns to the browser. HOME can request the usual ordered exit. There is no
ten-second dismissal timer. Normal completion and manual Stop show no error.

The first page explains the problem and suggests a next step. Later pages retain
the original internal error and, for a matching Sony AVC API failure, its
hexadecimal return code. These screens work with Debug Mode off.

## Encoding and container explanations

The report preserves information from the track that was accepted, or an
actually rejected candidate when no track was accepted. It copies the values
before teardown; it never follows freed container pointers. A skipped
incompatible track cannot label a later accepted track incompatible.

Specific explanations cover:

- Unsupported video codecs, including HEVC, AV1 and VP9 when the container
  supplies the codec identifier. MKV now retains a bounded copy of CodecID.
- Reported dimensions above 720x480, and the existing MKV Baseline restriction
  above 480x272. The MP4 large Baseline path is unchanged.
- Missing/invalid H.264 setup headers, invalid dimensions and missing video.
- Missing audio, unsupported audio codecs, channel count or sample rate, and
  other audio header/timing rejections. Complex audio setup failures are kept
  general when the validator does not distinguish their cause.
- The existing frame-rate rejection, seek-index budget, malformed timing,
  read failures, allocation failures and display/service errors.

For a video-decoding failure with valid parsed SPS metadata, the report can
identify higher-than-8-bit video, non-4:2:0 color sampling, or interlaced pictures
and suggest a suitable conversion. It does not reject additional files, change
the decoder's retry sequence, or automatically alter their encoding parameters.

Details include available source profile/level, coded dimensions, bit depths,
chroma format, reference-frame count, CABAC, weighted prediction, 8x8 transform,
audio codec/rate/channels and observations of B/reference-B pictures. These are
source settings, not necessarily the headers after H264X compatibility rewrites.
Observed B pictures are counts since the last probe reset/seek, not the
encoder's configured maximum consecutive B-frame setting. Reference-B evidence
is reported as observed; its absence does not prove B-pyramid was disabled.
Details that were never successfully parsed remain unavailable.

Profile, level, CABAC and B-pyramid alone are not used to blame an unexplained
firmware failure. Generic decoder errors say the exact cause is unknown;
pipeline stalls also identify the uncertainty between storage, decoding and
other stages. Debug Mode is suggested for the next attempt, not enabled
automatically by an error.

## Battery Saver and startup

Battery Saver retains its existing stricter admission rules. Its rejection page
can explain the failed header rule (profile/level, bit depth, chroma, interlacing,
reference count, slice groups, weighting, 8x8 transform or List 1 defaults),
timing/bitrate check, reference B pictures, consecutive B pictures or failed
startup sampling/rewind. The first known failed check is preserved. Turning
Battery Saver off is offered as the next step, without promising that normal
playback will succeed.

Startup failures use the existing early debug-screen renderer, with clearer
instructions for missing configuration, skins, fonts and helper modules.
Module load/start failures retain their native code and path. The fatal
firmware quarantine screen still requires a power-off: it does not offer a
return to the browser while a worker owns hardware or retained buffers.

## Cost and limits

The report uses bounded static metadata and a bounded controller-stack buffer.
MKV CodecID retention adds 64 bytes per track, at most 2 KiB with the existing
32-track cap. No extra packet parsing, logging, filesystem access, worker,
per-frame formatting or heap allocation is introduced by these screens. Source
H.264 metadata is copied at close, plus a failed Battery Saver probe before its
existing reset. The display is repainted only when changing pages.

Recovery drawing continues to use the existing CPU-only LCD/TV surface path
after workers have joined. It neither acquires a possibly damaged GU semaphore
nor launches GU work, custom Media Engine/VME code, or a codec reset. Text uses
the built-in ASCII bitmap fallback, without allocating FreeType faces after an
allocation failure. It is English and uppercase; non-ASCII filename characters
appear as placeholders. Exception traps and a hung firmware service cannot
always reach a normal recovered-error screen.

## Validation

`tests/run_host_tests.sh` includes actual MP4/MKV track-admission fixtures and
report tests under AddressSanitizer/UndefinedBehaviorSanitizer. Checks cover
unsupported codecs, dimensions, missing headers, audio rejection, skipped bad
tracks, accepted MKV admission, retained metadata after teardown, reset between
movies, native-error matching, conservative cause descriptions and bounded
pagination with long tokens/random text. This host test disables Opus; it does
not validate Opus decoding or any firmware calls.

Native syntax checks against PSP SDK headers passed for the changed C/C++
sources (the unchanged PSP ABI assertion header in `main.cpp` was excluded
on the 64-bit host). Sample pages were rendered at 480x272 using the actual glyph/rasterizer
functions; bounds were checked for LCD, progressive TV and interlaced TV layouts.
These checks are not a PSP cross-build or hardware test.

Device checks still needed: deliberately reject an oversized/HEVC/surround
audio file; reject a Battery Saver file; browse all pages with Left/Right;
dismiss with X/Triangle and play another movie; test HOME; and repeat on
PSP-1000, PSP-3000 and TV output. Verify the encoding facts against the source
file and the native code against Debug Mode's log where available.
