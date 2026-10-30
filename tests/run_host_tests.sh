#!/bin/sh
set -eu
# Supply a PSPSDK include directory; this uses native gcc, never runs PSP code.
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sdk_include=${PSPSDK_INCLUDE:-${PSPSDK:-/usr/local/pspdev/psp/sdk}/include}
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
cc=${CC:-cc}
"$cc" -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-int-to-pointer-cast \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$sdk_include" -I"$repo/cooleyesBridge" -I"$repo/ppu" \
    "$repo/tests/test_diagnostics.c" "$repo/ppu/common/ppa_debug_state.c" \
    "$repo/ppu/mod/subtitle_readability.c" -o "$build/diagnostics"
"$build/diagnostics"
"$cc" -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-int-to-pointer-cast \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$sdk_include" -I"$repo/cooleyesBridge" -I"$repo/ppu" \
    -I"$repo/libmp4info.psp" -I"$repo/libmkvinfo.psp" \
    "$repo/tests/test_pause.c" -o "$build/pause"
"$build/pause"
"$cc" -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-int-to-pointer-cast \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$sdk_include" -I"$repo/ppu" \
    "$repo/tests/test_memory.c" -o "$build/memory"
"$build/memory"
"$cc" -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-int-to-pointer-cast \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$sdk_include" -I"$repo/ppu" -I"$repo/libbufferedio.psp" \
    -I"$repo/libmp4info.psp" -I"$repo/libmkvinfo.psp" \
    -DPPU_ENABLE_OPUS=0 \
    "$repo/tests/test_playback_errors.c" "$repo/ppu/common/ppa_playback_error.c" \
    "$repo/ppu/mod/mp4_file.c" "$repo/ppu/mod/mkv_file.c" \
    "$repo/ppu/mod/audio_format.c" "$repo/ppu/common/ppa_utf8.c" \
    -o "$build/playback_errors"
"$build/playback_errors"
"$cc" -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-int-to-pointer-cast \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$sdk_include" -I"$repo/ppu" \
    "$repo/tests/test_io_standby.c" -o "$build/io_standby"
"$build/io_standby"
