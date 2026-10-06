# PMPlayer Ultraviolet

Sony PSP homebrew MP4/MKV player using Sony AVC/AAC/MP3 services, software
FLAC/Opus audio, an H264X compatibility layer, subtitles, and an automatic CPU/bus clock governor. PPU
aims for smooth, power-efficient playback by adapting clocks to the workload
and reducing unnecessary processing and frame copies.

Based on PMPlayer Advance by Cooleyes, and the original PMPlayer by neur0n. Much thanks to the both of them and the homebrew community at large!

This has been tested extensively on my real PSP 3000 model, using a combination of my own visual analysis and device benchmarking + analysis. I have a special branch with an extensive benchmarking and debug data collection that drove a lot of this development, but has been stripped out of this public release because most of it deliberately does not ship in release builds, and it's not pretty at the moment, making the codebase ugly to read through. I'll eventually merge it in when I have the time to improve the debug code's integration with project and make it suitably pretty for public debut!

Please note: TV Out has presently not been tested, I do not have the AV cables or interest at this time but will be happy to accept issue reports and do my best to fix them.

Developers: Building this project from source has been tested on OpenSUSE Tumbleweed Linux. I have also included converted powershell versions of the build accessory bash scripts, but they are not yet tested. You may have better luck using WSL as the scripts are not overly complex. You are welcome and encouraged to submit pull requests, I will review them as soon as I can and merge when they meet my fairly reasonable criteria (currently not standardized for this project so use best judgement). I might make some improvements myself, while still giving you full credit for the feature add.

## Feature list

- **Containers:** MP4 and MKV. I strongly recommend using MKV as it's quite literally works best during my tests and is inherently a better container spec.
- **Video:** H.264/AVC, progressive 8-bit 4:2:0, Main or Baseline profile, up to 720×480 (MKV Baseline limited to 480×272). Whenever possible use 480 x 272, that's the actual PSP screen resolution, higher is only beneficial for TV out.
- **H264X compatibility layer:** plays multiple consecutive B-frames and strict B-pyramid beyond standard PSP AVC limits, allowing better quality or smaller files depending on your converting preference. This is in beta.
- **Audio:** firmware AAC/MP3 plus software FLAC/Opus, with bounded mono/stereo configurations and compatible multi-track switching; see the format limits below.
- **44100 Hz output:** persistent libsamplerate conversion with a PSP constant-rate linear fast path and packed VFPU PCM conversion for admitted non-44100 Hz tracks; direct PCM for 44100 Hz sources. Fastest sinc remains a build-time opt-in pending device measurements.
- **Subtitles:** embedded (MP4 timed text, MKV SubRip/ASS/SSA/WebVTT) and external SRT/MicroDVD/ASS/SSA sidecars
- **Chapters:** L/R chapter navigation with a live target and 300 ms burst settling (MKV requires a cue index)
- **Seek:** one keyframe-based jump on each Left/Right press, then four fixed-size jump ticks per second while held; default jumps are bounded to 5–30 s
- **Aspect-ratio cycling, 180° rotation and brightness adjustments**
- **Audio Only Mode:** audio playback with display auto-off after 30 s inactivity
- **Automatic CPU/bus clock governor** with bounded post-seek boosts and epoch-filtered audio recovery
- **Controls Type:** International (X confirms, O cancels) or Asia (O confirms, X cancels), initially detected from the PSP region
- **Sleep Timer:** 15–720 min, pauses playback and requests native suspend
- **Debug Playback Health overlay:** clocks, decode budget, A/V delta, skipped frames
- **Translation:** 18 interface languages, with bounded bundled-font fallback for mixed-script filenames independent of the interface language; complex-script rendering remains in beta
- **Input-first file browser:** cached folders appear immediately; all folder scans and metadata probes wait for two seconds of input idle. A bounded text cache reuses rendered filenames during cursor movement.
- **Now featuring multi-partition and NTFS support!** NTFS appears in this player only, as ms1:// in the file browser. You can shrink your main PSP fat32 partition and add a second large NTFS partition for large files / movies etc. NTFS is much more safe against random corruption than FAT, with decent cross platform support.

## Install and play

TL;DR Download the latest release zip, extract it, copy the folder extracted to your /PSP/GAME/ folder on the PSP. Safely remove / eject before cable disconnect is always important with the PSP, never just pull the cable.

**Instructions for building from source instead:**
The source ZIP opens directly to `Makefile`, `README.md`, `ppu/`, the local
libraries, `tools/`, `docs/` and `licenses/`. Extract it into a project directory,
complete the [developer dependency setup](docs/README.md), then run `make`
there; the installable application is staged in `dist/PPU-1.0/`.

1. Use a PSP with custom firmware able to run this homebrew and its required
   PRXs. This ZIP is the **latest source snapshot**. Build and stage the
   application using [the developer instructions](docs/README.md) before copying
   the resulting `dist/PPU-1.0/` folder to the PSP. No new EBOOT or bridge
   binary was built for this source release.
   Running `make` at the archive root builds and stages that folder; staging
   retains the `ppa-fallback-*` font filenames used by the player.
2. Copy the complete application folder into `ms0:/PSP/GAME/PPU/`. Keep
   EBOOT, all `.prx` files, `config.xml`, `credit.png`, `fonts/`, `skins/`,
   `ui/` and the remaining staged assets together. If upgrading, preserve your
   old `config.xml` separately before replacing the installation.
3. Put compatible `.mp4` or `.mkv` files on accessible storage. The supplied
   configuration initially browses `ms0:/PSP/VIDEO/`; movies can live in other
   folders. Launch PPU from the PSP's Game menu.
4. Use Up/Down to select a folder, **Confirm** to enter it, and the `..` entry
   to go up. **Start** returns to the device list. Select a movie and press
   **Confirm** to start from the beginning, or the other X/O button to resume
   its saved position when available. Confirm is **X** for International and
   **O** for Asia.
5. During playback, **Square** pauses/resumes and **Triangle** stops and
   returns to the browser. In the browser, **Square** opens Configuration.

## Memory-card and partition setup

PPU reads the Memory Stick's first primary partition as FAT32 (for the
application and configuration) and its second primary partition as NTFS
(read-only, for movies). The scanner looks for the second MBR entry as NTFS,
then falls back to other primary entries of type `0x07`. GPT, extended/logical
partitions and exFAT are **not** supported.

Back up the card before repartitioning. PPU does not partition or format the
card; prepare the layout on a computer.

### Windows (Disk Management / `diskpart`)

1. Insert the Memory Stick. Open **Disk Management**
   (`diskmgmt.msc`) or `diskpart` in an elevated Command Prompt.
2. Delete all existing partitions on the stick.
3. Create **Partition 1** — size it for your PSP boot/application files
   (2 GB is plenty). Format as **FAT32** (32 KB cluster is fine).
   Assign a drive letter if desired.
4. Create **Partition 2** — use the remaining space. Format as **NTFS**.
5. Copy the PPU folder into `PSP\GAME\PPU\` on Partition 1.
6. Copy your movies into Partition 2 (any folder you like).

`diskpart` equivalent:

```text
diskpart
list disk
select disk N
clean
create partition primary size=2048
format fs=fat32 quick
assign
create partition primary
format fs=ntfs quick
assign
exit
```

### Linux (GParted / `fdisk` / `mkfs`)

Using `fdisk` and the `mkfs` utilities:

```bash
# Identify the stick (e.g. /dev/sdX). Back it up first!
sudo fdisk /dev/sdX

# Inside fdisk:
#   p            – print existing layout
#   d            – delete each existing partition (repeat as needed)
#   n            – new partition
#     p          – primary
#     1          – partition number 1
#     2048       – first sector (default start is fine)
#     +2G        – size: 2 GiB
#   n            – new partition
#     p          – primary
#     2          – partition number 2
#     (default)  – first sector (auto)
#     (default)  – last sector (rest of disk)
#   w            – write table and exit

# Format
sudo mkfs.vfat -F 32 /dev/sdX1
sudo mkfs.ntfs -F /dev/sdX2

# Mount and copy
sudo mkdir -p /mnt/psp /mnt/pspvids
sudo mount /dev/sdX1 /mnt/psp
sudo mount -o ro /dev/sdX2 /mnt/pspvids   # NTFS mounted read-only for testing
cp -r PPU /mnt/psp/PSP/GAME/
cp -r ~/Videos/Movies/* /mnt/pspvids/
sudo umount /mnt/psp /mnt/pspvids
```

Or use **GParted** graphically: erase the partition table, create a primary
FAT32 partition (≈2 GB), then a primary NTFS partition (remainder), apply,
format both, and copy files as above.

Once the card is in the PSP, by default only the fat32 partition will be visible to regular apps. But PMPLayer will be able to see it as ms1:// in it's file browser (vs ms0 for the fat32 partition).

> **Note:** NTFS access in PPU is **read-only**.

## Player controls

**Controls Type** in Configuration sets the semantic X/O buttons:

| Controls Type | Select / Confirm / Yes / OK | Cancel / No |
| --- | --- | --- |
| International | Cross (X) | Circle (O) |
| Asia | Circle (O) | Cross (X) |

On the first launch without a saved `controls_type`, Japan, Korea, Hong Kong,
Taiwan and China regions select Asia. A failed or unrecognized region query
selects International. A saved `International` or `Asia` value takes precedence.
The setting is stored on the `<player>` element in `config.xml`; deleting only
that attribute re-detects the default next launch. A missing config file starts
from defaults; an existing malformed file is reported without replacing it.

L/R mean the shoulder triggers. **Most single-button actions fire on release.**
**Combinations fire once as soon as all their buttons are held**, in either
press order; a slight offset is fine. Their releases are consumed, so X+Square
changes subtitle colour without also pausing. Browser Up/Down and playback
Left/Right fire on press and repeat while held; other singles do not repeat.
Physical X-modifier playback combinations stay the same in
both control types. No analog-stick playback/navigation binding is implemented.

### Browser

| Button | Action |
| --- | --- |
| Up / Down | Previous / next entry immediately on press, wrapping at the ends; held repeat starts after 350 ms, then every 80 ms. |
| Confirm (International: X; Asia: O) | Open selected folder, or play selected movie from the beginning. Select `..` to go up. |
| Other X/O button (International: O; Asia: X) | Play selected movie using its saved resume position, if available. |
| Start | Open the device/root list. |
| Square | Open Configuration. |
| Triangle | Show button help; Triangle again closes it. |
| Select | Ask to delete the selected movie. Confirmation can also remove matching subtitle/PNG companions. NTFS deletion is disabled. |
| L + Up / Down | First / last browser entry. |
| L + Square | Toggle selected-movie preview/metadata loading. |
| L + Triangle | Show version information; Triangle closes it. |
| L + Start | Ask to quit PPU. |
| Cross + Triangle | Switch between FAT `ms0:/` and detected NTFS `ms1:/`. |

### Configuration and prompts

| Context / button | Action |
| --- | --- |
| Configuration: Up / Down | Move between settings. |
| Configuration: L / R | First / last setting. |
| Configuration: Confirm | Edit the selected setting. |
| Editing: directional buttons | Change the value (see per-setting notes). |
| Editing: Confirm / Cancel | Commit / cancel the edit. Controls Type changes take effect after committing with the old mapping. |
| Configuration list: Square or Triangle | Close the menu; committed changes saved to `config.xml`. |
| Yes/No or OK/Cancel prompt: Confirm / Cancel | Yes or OK / No or Cancel; button hints reflect Controls Type. |

### Normal MP4/MKV video playback

| Button | Action |
| --- | --- |
| Square | Pause / resume. |
| Triangle | Stop and return to the browser. |
| Left / Right | One backward / forward jump on press, then four equal jump ticks per second while held; release adds no jump. |
| L alone / R alone | Select previous / next chapter on release. A burst updates the chapter number and seeks once after settling. MKV requires a cue index. |
| Circle while playing | Toggle the playback information interface. |
| Circle while paused | Save a PNG screenshot to `ms0:/PICTURE/PPU/`. |
| Select | Cycle compatible audio tracks. |
| Up / Down | Increase / decrease software volume boost (0–6). |
| Start | Cycle display aspect ratio. |
| Cross + Up / Down | Zoom in/out in 5 % steps (100–200 %). |
| Cross + R | Toggle looping of the current movie. |
| Cross + L | Cycle subtitle tracks, including Off. |
| Cross + Square | Cycle subtitle text colour. |
| Cross + Circle | Cycle subtitle border colour. |
| Cross + Select | Cycle audio channel (stereo / mono). |
| Cross + Start | Toggle 180° output rotation. |

The default seek jump is about 1/120 of the movie duration, bounded to
**5–30 seconds** (10 s when duration is unknown). These are keyframe-based
seeks; arrival depends on the file's index. Each direction press/held tick adds
one jump to the requested target and is eligible for immediate consumption.
If a decoder seek is already in flight, ticks accumulate into one replacement
target until that epoch arrives, avoiding a firmware-reset backlog. The held
rate is fixed at four ticks per second, without acceleration or catch-up bursts.
Chapter taps update the selected chapter number immediately and retain their
**300 ms** release/quiet settling window.

L/R **alone always go to the chapter handler**. Battery Saver disables the
legacy luminosity-boost fallback on L/R. The skin chooser and L+Select shortcut
have been removed; skins remain runtime assets selected through `config.xml`.

L+Triangle displays:

```text
PMPlayer Ultraviolet
Version 1.0
```

### Audio Only Mode

Enable in Configuration before opening a file. Starts from the beginning, no
video decoding, seeking, resume, chapters or subtitles.

| Button (on release) | Action |
| --- | --- |
| Square | Pause / resume audio. |
| Triangle | Stop and return to the browser. |
| Select | Cycle compatible audio tracks. |
| L / R | Decrease / increase software volume boost. |
| Any input after display-off | Wake the display (first input consumed). |

Display turns off after **30 seconds** without input.

## Settings and power saving

| Setting | Notes |
| --- | --- |
| **Controls Type** | International / Asia; region-detected on first launch and overridden by the saved setting. |
| **Auto CPU Clock** | Enabled by default. Single governor adjusts CPU/bus clocks for playback load, including temporary boosts after seeks. |
| **CPU Speed** | 66 / 111 / 133 / 166 / 222 / 266 / 333 MHz. While Auto CPU Clock is on, this is the idle/manual floor, not a playback lock. |
| **Battery Saver** | Off by default. Strict native-video admission and H264X bypass; bounded clock-spike suppression, cheaper subtitle shadow and linear audio resampling. In a sinc-enabled build it selects the cheaper converter at stream open; the default build already uses linear. Existing CBR-like audio gates remain. **Keep off for B-frames / strict-pyramid playback.** |
| **Fit PSP Screen** | Optional LCD panel color/range compensation, packaged off; bypassed on TV output and in Battery Saver. |
| **Playback Health** | Off by default. Persistent cached overlay with clocks, decode time/budget, estimated A/V delta and skipped frames. Metrics refresh at most once per second while video is rendering. |
| **Video Output** | PSP LCD, composite, component interlaced or component progressive, subject to model/cable/driver support and reserved TV buffers. |
| **Sleep Timer** | Off, or 15–720 min in 15-min steps. Pauses playback and requests native suspend. Wake leaves playback paused; Square resumes. |
| **Audio Only Mode** | Off by default. See its separate controls above. |

For low-overhead LCD playback, a progressive 480×272-or-smaller encode with
automatic clocks and Fit PSP Screen off is a good starting point. Actual
power use depends on the encode, storage, subtitles, display and PSP model.
Seek selection does not repeatedly request 333 MHz; the actual seek receives a
bounded recovery boost, then ordinary governor feedback can reduce clocks again.
These changes have received source review only, with no build or PSP playback
run in this session; the reported seek/A/V recovery still needs device confirmation.

Every browser folder scan, including an uncached folder's first listing, waits
for **two seconds without input**. Presses, holds and releases restart the quiet
period. Cached folders appear immediately; without a cached listing, the current
list stays usable until the requested directory is ready. Reads pause at their
next checkpoint when navigation resumes, and refreshes preserve the current
selection and scroll position. Repeated filename drawing uses a bounded RAM
cache; periodic redraws and metadata display also wait for input idle.
Cancellation is cooperative: an already running firmware read must return first,
and first-use font rendering can still read the card. See the
[browser responsiveness notes](docs/BROWSER_RESPONSIVENESS.md) for scope and limits.

## Supported media and transcoding

| Item | Requirement |
| --- | --- |
| **Video codec** | H.264/AVC, progressive **8-bit 4:2:0**, Main or Baseline profile. HEVC, AV1, VP9, MPEG-4 Part 2 are not supported. |
| **Resolution** | Up to **720×480** (MP4). MKV Baseline is limited to **480×272**. For consistent cross-container use, stay at or below 480×272 or use Main for larger frames. |
| **AAC-LC / MP3** | Mono/stereo, 22 050 / 24 000 / 32 000 / 44 100 / 48 000 Hz. Firmware decoding. |
| **FLAC** | Mono/stereo, 8–48 kHz, 4–24 bits, native frames with STREAMINFO; maximum 8192 PCM frames per packet. Higher depths are rounded and saturated to 16-bit output. |
| **Opus** | Mapping family 0 mono/stereo, decoded at 48 kHz then resampled to 44.1 kHz; PSP-target libopus required. MP4 requires the supported single-entry edit list for trimming. |
| **Audio limits** | Compressed packets at most 128 KiB. No AC-3/DTS, surround, standalone FLAC/Ogg/Opus playback or FLAC above 48 kHz. Detailed container/trim limits: [audio guide](docs/AUDIO_RESAMPLING_AND_CODECS.md). |
| **Multi-track audio** | Tracks must share codec, sample rate and channel count to be switchable. |
| **MP4 layout** | Conventional, preferably interleaved, classic sample tables. FLAC/Opus accept one nonempty unit-rate audio edit; fragmented MP4 and general edit lists remain unsupported. |
| **MKV layout** | Conventional AVC plus admitted AAC/MP3/FLAC/Opus, with a **Cues** index for seeking, chapters and resume. |
| **Subtitles** | MP4: `mov_text`/`tx3g` timed text. MKV: SubRip, ASS/SSA, WebVTT. External: SRT, text MicroDVD, ASS, SSA (same-stem). |

### H.264 encoding options

A) Most stable options, use **Main profile**
with:

```
bframes=1:b-pyramid=none:ref=1:cabac=1:weightp=0:weightb=0:8x8dct=0
```

B) New better H624 encoder support for B-Pyramid and multiple b-frames: **Main profile**
with:

```
bframes=3:b-pyramid=strict:ref=1:cabac=1:weightp=0:weightb=0:8x8dct=0
```

C) For maximum battery life with lower quality: **Baseline** (no B-frames, no CABAC):

```
bframes=0:b-pyramid=none:ref=1:cabac=0:weightp=0:weightb=0:8x8dct=0
```

Key rules:

- **Do not use `b-pyramid=normal`** — it's buggy at the moment.
- **Keep `ref=1`.** Higher reference counts are not validated.
- **`8x8dct=0`** (4×4 transforms). 8×8 / High-profile files are not supported.
- Weighting b / p, is partially supported but can cause minor artifacts; the
  clean7 build applies a lossy weights-off decoder copy to mitigate green
  flashes on eligible streams.
- High profile is not supported at all.

Suggested conversion steps:

1. I recommend ffmpeg, but you can interpret the options I just described above into any video converter.
2. Go with the PSP's native screen resolution of 480 x 272 for best playback stability and battery life. However it can also play 720 x 480 - but unless you use TV out, this is pointless and gets downscaled to 480 x 272 anyways.
3. I have found encoder option set B above to work pretty well and it allows you to use more aggressive conversion settings to get smaller files, before converting your whole library maybe try a few conversions and watch carefully to see how it fares for you. Some minor artifacts will occur during playback of this option, it's very much beta support, but it's mostly unnoticable during my tests so far. Option C is nice if you really don't care about maximum quality or larger video files, it will require less power to decode so the PSP battery should last a lot longer.

## Subtitles

Use **Cross + L** to cycle subtitle tracks and Off.

- **MKV:** initial embedded track chosen by Subtitle Language, then
  forced/default flags.
- **MP4:** up to four embedded subtitle tracks; the separated-track path does
  not expose embedded subtitles.
- **External files:** same stem as the video (e.g. `Movie.mp4` →
  `Movie.srt`). Supported: `.srt`, `.sub` (text MicroDVD), `.ass`, `.ssa`.
  Case-insensitive matching.
- **Language suffixes:** `.en`/`.eng` → UTF-8, `.ch`/`.gb`/`.chs`/`.sc` →
  GBK, `.big5`/`.cht`/`.tc` → BIG5. Unsuffixed files use the Subtitle
  Encoding setting (default UTF-8). UTF-8/UTF-16 BOMs are auto-detected.
- **NTFS movies:** prefer embedded text subtitles; external sidecar discovery
  on NTFS is not fully established.
- **ASS/SSA:** text-oriented playback. Override tags, positioning, drawings
  and karaoke are not a full desktop ASS renderer. Use plain SRT or burn
  complex styled subtitles into the video.
- Configuration exposes subtitle language, encoding, size, bold, alignment and
  margin. Cross + Square / Circle changes text/border colour during playback.

## Storage backends

| Path | Description |
| --- | --- |
| `ms0:/` | PSP-native FAT Memory Stick; application, config, screenshots, bookmarks. |
| `ef0:/` | Native internal storage where the firmware provides it. |
| `ms1:/` | PPU's **read-only NTFS view** of the second partition. Switch with Cross + Triangle. |

FAT32 per-file limit is **4 GiB − 1 byte**. PPU preserves 64-bit file
addresses, so a capable NTFS backend can address larger files, subject to RAM
and metadata-table caps (8 MiB cumulative MP4 metadata, 12 MiB combined
sample/seek indexes).

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Blocking, green flashes, other artifacts | Use strict/none pyramid, `ref=1`, no weights, 4×4 transforms, Main/Baseline. Keep Battery Saver off. |
| Decoder error at startup | Check codec, profile, bit depth, interlacing, dimensions. Transcode High/8×8 or HD video. |
| Battery Saver rejects a playable movie | It uses stricter admission and bypasses H264X. Turn it off. |
| Missing/unsupported audio | Check codec/rate/channel/packet and edit-list limits in the audio guide, plus the libopus build option. Match properties across switchable tracks. AAC-LC at 44100 Hz remains a firmware-decoded fallback. |
| Subtitles absent or garbled | Same-stem name, text format, Subtitle Encoding, installed fonts, Cross + L. Use embedded text for NTFS. |
| Seek/chapter unavailable | Include MKV Cues and chapter metadata, or a conventional indexed MP4. |
| Display dark / playback paused after wake | Check Audio Only Mode and Sleep Timer. Wake input is consumed; sleep-timer wake leaves playback paused. |
| Bridge/API mismatch or incomplete install | Rebuild EBOOT, bridge API 3 and metadata libraries together; install the complete staged package. |

## Developer notes

Architecture highlights:

- **cooleyesBridge (API 3):** Cooleyes-derived kernel bridge for Sony ME boot,
  audio frequency, display power and region queries. H264X processing lives in
  the player's main-CPU code; this bridge uploads no custom ME/VME program.
- **H264X:** not a software decoder; it still relies on Sony firmware
  decoding. It extends what the firmware path will accept.
- **Clock governor:** single adaptive governor for CPU/bus clocks with
  post-seek temporary boosts.
- **miniconv:** charset conversion for subtitles (UTF-8, GBK, BIG5, etc.).
- **i18n:** 18-language interface with bundled font assets.
- **libyal / libfsntfs family:** NTFS read support via `ms1:/`.
- **FreeType / HarfBuzz / FriBidi:** font rasterization, script shaping,
  bidirectional text.
- **libpng / zlib / bzip2:** UI/skin PNG loading and compression.
- **Boost (selected headers):** noncopyable owners, scoped modal cleanup, a
  fixed pointer array and compile-time ABI/layout checks; no linked Boost library.
- **libsamplerate / dr_flac / libopus:** shared persistent audio conversion,
  bounded FLAC decoding and PSP-target Opus packet decoding on the existing
  decode owner; no additional audio thread or clock governor.

Phase-one work on shared session/timeline, frame ownership/copy reduction and
large-file handling has completed; later phases are still in progress.

## Credits
- **cooleyes** (`eyes.cooleyes@gmail.com`) — original PMPlayer / PPU author
  and core library maintainer.
- **neur0n** — original PMPlayer creator, acknowledged in the launch tribute.
- **jonny (2006)** — PMP Mod rendering, aspect/texture subdivision,
  movie interface.
- **Raphael (2006)** — PMP Mod bitmap-font and subtitle parsing, MicroDVD /
  SubRip portions.
- **Lee Thomason** — TinyXML (zlib-style licence).
- **Boost contributors** — selected Core, SmartPtr, Array, StaticAssert and
  TypeTraits headers (Boost Software License 1.0; original notices retained).
- **Erik de Castro Lopo and libsamplerate contributors** — sample-rate conversion
  (BSD-2-Clause; supplied 0.2.2 source/license).
- **David Reid and dr_libs contributors** — dr_flac decoding (upstream
  dual-choice license text retained in `licenses/dr_libs-LICENSE.txt`).
- **Xiph.Org, Skype, the Opus authors and contributors** — external libopus
  decoder; retain the original notices from the exact PSP-target dependency.
- **PSPDEV contributors** (adresd, Marcus R. Brown, James Forshaw, John
  Kelley, Jesper Svennevid, et al.) — PSPSDK.
- **Sorin P. C. (2006)** — local `pspmpeg.h` and import stubs.
- **Dark_AleX / CFW SDK authors** — M33 SDK / KUBridge.
- **Sony Computer Entertainment** — AVC/AAC/MP3 firmware services.
- **David Turner, Robert Wilhelm, Werner Lemberg, FreeType Project** —
  FreeType (FTL / GPL option).
- **Behdad Esfahbod, Google, Red Hat, SIL, contributors** — HarfBuzz (MIT).
- **Dov Grobgeld, Behdad Esfahbod, contributors** — FriBidi (LGPL-2.1+).
- **Guy Eric Schalnat, Andreas Dilger, Glenn Randers-Pehrson, et al.** —
  libpng.
- **Jean-loup Gailly, Mark Adler** — zlib.
- **Julian Seward, contributors** — bzip2.
- **Joachim Metz, libyal contributors** — libfsntfs and the libyal library
  family (LGPL-3.0+).
- **newlib contributors** — C runtime.
- **FSF / GCC contributors** — `libgcc` / `libstdc++` (GPL + Runtime
  Exception).
- **Bitstream / Tavmjong Bah / DejaVu contributors** — DejaVu Sans font.
- **Google, Adobe, Monotype, Noto CJK contributors, WenQuanYi Board** —
  Noto / WQY fonts (SIL OFL 1.1 / Apache 2.0).
- **Just van Rossum, Behdad Esfahbod, FontTools contributors** — FontTools /
  `pyftsubset` (MIT), workstation font-subsetting only.
- **FFmpeg / x264 contributors** — workstation fixture conversion only; no
  decoder code bundled.
- **PSPDEV GCC/binutils, GNU make, Python** — build toolchain.
- **AI** was used extensively to refactor the original codebase, with extensive data collection based testing performed on my real PSP.

Component licences attach to each individual component; this README does not
relicense them. Original source notices remain intact in the code. For
GPL/LGPL binaries, also satisfy the applicable corresponding-source and
relinking requirements for the exact linked libraries and modifications.
