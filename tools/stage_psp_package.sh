#!/bin/sh
set -eu
ppu=${1:?ppu root}
dist=${2:?dist root}
config=${3:-config-en.xml}
miniconv=${4:-miniconv.eur.prx}
i18n=${5:-i18n.eng.prx}
font_subsets=${6:?generated subtitle fonts}
project_root=$(CDPATH= cd -- "$ppu/.." && pwd)

# Exact build products only: recursive searches can silently select an old
# probe-only PRX from a different build directory or a previous package.
eboot="$ppu/EBOOT.PBP"
cooleyes="$ppu/../cooleyesBridge/cooleyesBridge.prx"
for file in "$eboot" "$cooleyes" \
            "$ppu/extra/$config" "$ppu/extra/credit.png" \
            "$ppu/miniconv/$miniconv" "$ppu/i18n/$i18n" \
            "$project_root/THIRD_PARTY_NOTICES.txt" "$project_root/COPYING" \
            "$project_root/LICENSE" "$project_root/SOURCE_INFO.txt"; do
    [ -f "$file" ] || { echo "Missing required package input: $file" >&2; exit 3; }
done
# These filenames are shared with the generator and runtime font lookup.
# Keep the compatibility prefix even though the application is now PPU.
[ -f "$font_subsets/ppa-fallback-core.ttf" ] || { echo "Missing subtitle font subsets" >&2; exit 3; }
[ -f "$project_root/licenses/INDEX.txt" ] || { echo "Missing project license inventory" >&2; exit 3; }
mkdir -p "$dist/fonts" "$dist/skins" "$dist/ui" "$dist/licenses"
# Only local playback/display helpers are eligible for packaging.
for driver in dvemgr.prx mpeg_vsh330.prx mpeg_vsh350.prx mpeg_vsh370.prx; do
    file="$ppu/extra/$driver"
    [ ! -f "$file" ] || cp -f "$file" "$dist/"
done
cp -f "$eboot" "$dist/EBOOT.PBP"
cp -f "$cooleyes" "$dist/cooleyesBridge.prx"
cp -f "$ppu/extra/$config" "$dist/config.xml"
cp -f "$ppu/extra/credit.png" "$dist/credit.png"
cp -f "$ppu/miniconv/$miniconv" "$dist/miniconv.prx"
cp -f "$ppu/i18n/$i18n" "$dist/i18n.prx"
for directory in fonts skins ui; do
    cp -R "$ppu/extra/$directory/." "$dist/$directory/"
done
cp -f "$font_subsets"/ppa-fallback-* "$dist/fonts/"
# Runtime distributions need the same copyright/license notices as source.
# This copies supplied texts; it does not certify unresolved binary provenance.
cp -R "$project_root/licenses/." "$dist/licenses/"
cp -f "$project_root/THIRD_PARTY_NOTICES.txt" "$dist/THIRD_PARTY_NOTICES.txt"
cp -f "$project_root/COPYING" "$dist/COPYING"
cp -f "$project_root/LICENSE" "$dist/LICENSE"
cp -f "$project_root/SOURCE_INFO.txt" "$dist/SOURCE_INFO.txt"
printf '%s\n' 'EBOOT.PBP' 'cooleyesBridge.prx' \
    'config.xml' 'credit.png' 'miniconv.prx' 'i18n.prx' 'fonts/' 'skins/' 'ui/' 'licenses/' \
    'THIRD_PARTY_NOTICES.txt' 'COPYING' 'LICENSE' 'SOURCE_INFO.txt' > "$dist/PACKAGE_CONTENTS.txt"
